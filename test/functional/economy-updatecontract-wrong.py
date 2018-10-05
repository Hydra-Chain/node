#!/usr/bin/env python3
# Copyright (c) 2017 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

""" LockTrip Economy smart contract functional test

This test checks the functionality of the LockTrip Economy smart contract.
It will perform a check that if a contract is updated with wrong data, it shall fail
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

def create_unsigned_pos_block(self, staking_prevouts, nTime):

    best_block_hash = self.node.getbestblockhash()
    block_height = self.node.getblockcount()

    parent_block_stake_modifier = int(self.node.getblock(best_block_hash)['modifier'], 16)
    parent_block_raw_hex = self.node.getblock(best_block_hash, False)
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

    stake_tx_unsigned.vin.append(CTxIn(coinstake_prevout))
    stake_tx_unsigned.vout.append(CTxOut())
    stake_tx_unsigned.vout.append(CTxOut(int(10002*COIN), scriptPubKey))
    stake_tx_unsigned.vout.append(CTxOut(int(10002*COIN), scriptPubKey))

    stake_tx_signed_raw_hex = self.node.signrawtransaction(bytes_to_hex_str(stake_tx_unsigned.serialize()))['hex']
    f = io.BytesIO(hex_str_to_bytes(stake_tx_signed_raw_hex))
    stake_tx_signed = CTransaction()
    stake_tx_signed.deserialize(f)
    block.vtx.append(stake_tx_signed)

    return (block, block_sig_key)

class EconomyTest(BitcoinTestFramework):

    def set_test_params(self):
        """Override test parameters for your individual test.

        This method must be overridden and num_nodes must be exlicitly set."""
        self.setup_clean_chain = False
        self.num_nodes = 1
        self.extra_args = [[]]

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

        scriptPubKey = CScript([OP_0, ser_uint256(witness_hash)])

        ### TODO: CREATE THE TX
        addcontract_tx = CTransaction()
        addcontract_tx.vin.append(CTxIn(prevout, b""))
        child_value = int(value/NUM_OUTPUTS)
        for i in range(NUM_OUTPUTS):
            addcontract_tx.vout.append(CTxOut(child_value, scriptPubKey))
        addcontract_tx.vout[0].nValue -= 50000
        assert(addcontract_tx.vout[0].nValue > 0)
        addcontract_tx.rehash()

        staking_prevouts = []

        for unspent in self.nodes[1].listunspent():
            if unspent['confirmations'] > COINBASE_MATURITY:
                staking_prevouts.append((COutPoint(int(unspent['txid'], 16), unspent['vout']), int(unspent['amount'])*COIN))

        t = int(time.time()) & 0xfffffff0
        (block, block_sig_key) = create_unsigned_pos_block(staking_prevouts, t)
        block.vtx.extend([addcontract_tx])

        # Add the witness commitment to the coinbase,
        # since it is a PoS block, the witness hash for the coinstake is 0
        add_witness_commitment(block, 0, is_pos=True)

        block.sign_block(block_sig_key)
        block.rehash()

        block_count = self.nodes[0].getblockcount()
        self.nodes[0].submitblock(bytes_to_hex_str(block.serialize(with_witness=True)))

        # The block shall not be accepted
        assert_equal(self.nodes[0].getblockcount(), block_count)


if __name__ == '__main__':
    EconomyTest().main()
