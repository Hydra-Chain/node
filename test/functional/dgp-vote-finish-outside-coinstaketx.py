#!/usr/bin/env python3
# Copyright (c) 2017 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

""" LockTrip Economy smart contract functional test

This test checks the functionality of the LockTrip Economy smart contract.
It will perform a check that if a contract is added to block and
Economy AddContract is called in coinstake, the block shall be accepted
"""

from collections import defaultdict

# Avoid wildcard * imports if possible
from test_framework.test_framework import BitcoinTestFramework
from test_framework.qtum import *
from test_framework.blocktools import *
from test_framework.key import *
import io
import time

class DgpTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = False
        self.num_nodes = 1

    def collect_staking_prevouts(self):
        blocks = []
        for block_number in range(self.node.getblockcount()):
            block_hash = self.node.getblockhash(block_number)
            blocks.append(self.node.getblock(block_hash))

        staking_prevouts = []

        for unspent in self.node.listunspent():
            for block in blocks:
                if unspent['txid'] in block['tx']:
                    tx_block_time = block['time']
                    break
            else:
                assert(False)

            if unspent['confirmations'] > COINBASE_MATURITY:
                staking_prevouts.append((COutPoint(int(unspent['txid'], 16), unspent['vout']), int(unspent['amount'])*COIN, tx_block_time))

        return staking_prevouts

    def create_unsigned_pos_block(self, node, staking_prevouts, nTime=None, nCounter=0):
        tip = node.getblock(node.getbestblockhash())
        if not nTime:
            current_time = int(time.time()) + 16
            nTime = current_time & 0xfffffff0

        #print('node.getbestblockhash node=%s' % (tip))
        parent_block_stake_modifier = int(tip['modifier'], 16)
        coinbase = create_coinbase(tip['height']+1)
        coinbase.vout[0].nValue = 0
        coinbase.vout[0].scriptPubKey = b""
        coinbase.rehash()
        block = create_block(int(tip['hash'], 16), coinbase, nTime, nCounter=nCounter)
        block.hashStateRoot = int(tip['hashStateRoot'], 16)
        block.hashUTXORoot = int(tip['hashUTXORoot'], 16)

        if not block.solve_stake(parent_block_stake_modifier, staking_prevouts):
            print('create_unsigned_pos_block->block.solve_stake()->None')
            return (None, None)

        txout = node.gettxout(hex(block.prevoutStake.hash)[2:], block.prevoutStake.n)
        # input value + block reward
        out_value = int((float(str(txout['value'])) * COIN + INITIAL_BLOCK_REWARD * COIN)) // 2

        # create a new private key used for block signing.
        block_sig_key = CECKey()
        block_sig_key.set_secretbytes(hash256(struct.pack('<I', random.randint(0, 0xff))))
        pubkey = block_sig_key.get_pubkey()
        scriptPubKey = CScript([pubkey, OP_CHECKSIG])

        stake_tx_unsigned = CTransaction()

        stake_tx_unsigned.vin.append(CTxIn(block.prevoutStake))
        stake_tx_unsigned.vout.append(CTxOut())

        # Split the output value into two separate txs
        stake_tx_unsigned.vout.append(CTxOut(int(out_value), scriptPubKey))
        stake_tx_unsigned.vout.append(CTxOut(int(out_value), scriptPubKey))

        stake_tx_signed_raw_hex = node.signrawtransaction(bytes_to_hex_str(stake_tx_unsigned.serialize()))['hex']
        #print('stake_tx_unsigned.vout=%s' % (stake_tx_unsigned.vout))
        f = io.BytesIO(hex_str_to_bytes(stake_tx_signed_raw_hex))
        stake_tx_signed = CTransaction()
        stake_tx_signed.deserialize(f)
        block.vtx.append(stake_tx_signed)
        block.hashMerkleRoot = block.calc_merkle_root()
        return (block, block_sig_key)

    def run_test(self):
        self.node = self.nodes[0]
        self.node.setmocktime(int(time.time()) - 2*COINBASE_MATURITY)
        self.node.generate(50+COINBASE_MATURITY)

        staking_prevouts = self.collect_staking_prevouts()

        self.node.setmocktime(int(time.time()))

        CALLSTRING_CHAR_COUNT = 64

        # 1. Get admin addresses
        main_address = self.nodes[0].getnewaddress()
        backup_address = self.nodes[0].getnewaddress()
        main_address_eth = self.nodes[0].gethexaddress(main_address)
        backup_address_eth = self.nodes[0].gethexaddress(backup_address)

        self.nodes[0].sendtoaddress(main_address, 100)

        # 2. Set initial admin for DGP and Oracle

        callstring = ("7fd05e2a" +
                      "000000000000000000000000" + main_address_eth +
                      "000000000000000000000000" + backup_address_eth)

        self.nodes[0].sendtocontract("0000000000000000000000000000000000000091", callstring)
        self.nodes[0].sendtocontract("0000000000000000000000000000000000000092", callstring)

        self.nodes[0].generate(1)

        # 3. Create some sample vote callstring
        createvote_callstring = ("70eb3901" +
                                 "0" * (CALLSTRING_CHAR_COUNT - len(format(3, 'x'))) + format(3, 'x') +   # Burn rate param
                                 "0" * (CALLSTRING_CHAR_COUNT - len(format(33, 'x'))) + format(33, 'x') + # Param value
                                 "0" * (CALLSTRING_CHAR_COUNT - len(format(13, 'x'))) + format(5, 'x'))  # Vote duration

        # 4. Create vote
        self.nodes[0].sendtocontract("0000000000000000000000000000000000000091", createvote_callstring, 0, 2500000, main_address)

        self.nodes[0].generate(1)

        vote_for_callstring = "4b9f5c98" + "000000000000000000000000000000000000000000000000000000000000000" + "1" # vote for
        vote_against_callstring = "4b9f5c98" + "000000000000000000000000000000000000000000000000000000000000000" + "0" # vote against

        self.nodes[0].sendtocontract("0000000000000000000000000000000000000091", vote_for_callstring, 11, 2500000, main_address)
        self.nodes[0].sendtocontract("0000000000000000000000000000000000000091", vote_against_callstring, 10, 2500000, main_address)

        self.nodes[0].generate(2)

        block_count = self.nodes[0].getblockcount()

        finish_callstring = "2aebcbb6"

        self.nodes[0].sendtocontract("0000000000000000000000000000000000000091", finish_callstring, 0, 2500000, main_address)
        self.nodes[0].generate(1)

        assert_equal(self.nodes[0].getblockcount(), block_count + 1)

if __name__ == '__main__':
    DgpTest().main()
