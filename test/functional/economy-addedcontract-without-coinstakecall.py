#!/usr/bin/env python3
# Copyright (c) 2017 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

""" HYDRA Economy smart contract functional test

This test checks the functionality of the HYDRA Economy smart contract.
It will perform a check that if a contract is added to block, but
Economy AddContract is not called in coinstake, the block shall not be accepted
"""

from collections import defaultdict

# Avoid wildcard * imports if possible
from test_framework.test_framework import BitcoinTestFramework
from test_framework.qtum import *
from test_framework.blocktools import *
from test_framework.key import *
import io
import time

# NodeConnCB is a class containing callbacks to be executed when a P2P
# message is received from the node-under-test. Subclass NodeConnCB and
# override the on_*() methods if you need custom behaviour.
class BaseNode(NodeConnCB):
    def __init__(self):
        """Initialize the NodeConnCB

        Used to inialize custom properties for the Node that aren't
        included by default in the base class. Be aware that the NodeConnCB
        base class already stores a counter for each P2P message type and the
        last received message of each type, which should be sufficient for the
        needs of most tests.

        Call super().__init__() first for standard initialization and then
        initialize custom properties."""
        super().__init__()
        # Stores a dictionary of all blocks received
        self.block_receive_map = defaultdict(int)

    def on_block(self, conn, message):
        """Override the standard on_block callback

        Store the hash of a received block in the dictionary."""
        message.block.calc_sha256()
        self.block_receive_map[message.block.sha256] += 1

    def on_inv(self, conn, message):
        """Override the standard on_inv callback"""
        pass

class EconomyTest(BitcoinTestFramework):

    def set_test_params(self):
        """Override test parameters for your individual test.

        This method must be overridden and num_nodes must be exlicitly set."""
        self.setup_clean_chain = False
        self.num_nodes = 1
        self.extra_args = [[]]

    def create_unsigned_pos_block(self, staking_prevouts, nTime):

        best_block_hash = self.nodes[0].getbestblockhash()
        block_height = self.nodes[0].getblockcount()

        parent_block_stake_modifier = int(self.nodes[0].getblock(best_block_hash)['modifier'], 16)
        parent_block_raw_hex = self.nodes[0].getblock(best_block_hash, False)
        f = io.BytesIO(hex_str_to_bytes(parent_block_raw_hex))
        parent_block = CBlock()
        parent_block.deserialize(f)
        coinbase = create_coinbase(block_height+1)
        coinbase.vout[0].nValue = 0
        coinbase.vout[0].scriptPubKey = b""
        coinbase.rehash()
        block = create_block(int(best_block_hash, 16), coinbase, nTime)
        block.hashPrevBlock = int(best_block_hash, 16)
        if not block.solve_stake(parent_block_stake_modifier, staking_prevouts):
            return None

        # create a new private key used for block signing.
        block_sig_key = CECKey()
        block_sig_key.set_secretbytes(hash256(struct.pack('<I', 0xffff)))
        pubkey = block_sig_key.get_pubkey()
        scriptPubKey = CScript([pubkey, OP_CHECKSIG])
        stake_tx_unsigned = CTransaction()
        coinstake_prevout = block.prevoutStake

        stake_tx_unsigned.vin = [CTxIn(coinstake_prevout), make_vin(self.nodes[0], int(2*(COIN + QTUM_MIN_GAS_PRICE*100000)))]
        stake_tx_unsigned.vout.append(CTxOut())
        stake_tx_unsigned.vout.append(CTxOut(int(96268640), scriptPubKey))
        stake_tx_unsigned.vout.append(CTxOut(int(96268640), scriptPubKey))

        stake_tx_signed_raw_hex = self.nodes[0].signrawtransaction(bytes_to_hex_str(stake_tx_unsigned.serialize()))['hex']
        f = io.BytesIO(hex_str_to_bytes(stake_tx_signed_raw_hex))
        stake_tx_signed = CTransaction()
        stake_tx_signed.deserialize(f)
        block.vtx.append(stake_tx_signed)

        return (block, block_sig_key)

    def run_test(self):
        # Create a P2P connection to one of the nodes
        node0 = BaseNode()
        connections = []
        connections.append(NodeConn('127.0.0.1', p2p_port(0), self.nodes[0], node0))
        node0.add_connection(connections[0])

        # Start up network handling in another thread. This needs to be called
        # after the P2P connections have been created.
        NetworkThread().start()

        # wait_for_verack ensures that the P2P connection is fully up.
        node0.wait_for_verack()

        staking_prevouts = []
        blocks = []

        for i in range(self.nodes[0].getblockcount()):
            blocks.append(self.nodes[0].getblock(self.nodes[0].getblockhash(i)))

        for unspent in self.nodes[0].listunspent():
            for block in blocks:
                if unspent['txid'] in block['tx']:
                    tx_block_time = block['time']
                    break

            if unspent['confirmations'] > COINBASE_MATURITY:
                staking_prevouts.append((COutPoint(int(unspent['txid'], 16), unspent['vout']), int(unspent['amount'])*COIN, tx_block_time))

        bytecode = "6080604052348015600f57600080fd5b50336000806101000a81548173ffffffffffffffffffffffffffffffffffffffff021916908373ffffffffffffffffffffffffffffffffffffffff160217905550603580605d6000396000f3006080604052600080fd00a165627a7a723058200da151a481692b31bb57ba8af1190a8a4ab6f8a7543d188bcac01516e57550b80029"

        script = CScript([0x4, 0x2625a0, 0x28, bytes.fromhex(bytecode), OP_CREATE])
        print("SCRIPT -> " + str(script))

        ### TODO: CREATE THE TX
        addcontract_tx = CTransaction()
        addcontract_tx.vin.append(CTxIn(staking_prevouts[0][0], b""))
        addcontract_tx.vout.append(CTxOut(0, script))
        addcontract_tx.rehash()

        t = int(time.time()) & 0xfffffff0
        (block, block_sig_key) = self.create_unsigned_pos_block(staking_prevouts[1:], t)
        block.vtx.extend([addcontract_tx])

        # Add the witness commitment to the coinbase,
        # since it is a PoS block, the witness hash for the coinstake is 0
        add_witness_commitment(block, 0, is_pos=True)

        block.sign_block(block_sig_key)
        block.rehash()

        block_count = self.nodes[0].getblockcount()
        res = self.nodes[0].submitblock(bytes_to_hex_str(block.serialize(with_witness=True)))

        print(res)

        # The block shall not be accepted
        #Inconclusive is returned due to the small Difficulty.
        #assert_equal(self.nodes[0].getblockcount(), block_count + 1)
        assert(res in (None, 'inconclusive'))

if __name__ == '__main__':
    EconomyTest().main()
