#!/usr/bin/env python3
# Copyright (c) 2018 The LockTrip developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

""" LockTrip DGP functional test

This test checks the functionality of the LockTrip decentralized governance protocol.
It will perform a check that if DGP admin is set properly, it will set the initial data.
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

class DgpTest(BitcoinTestFramework):

    def set_test_params(self):
        """Override test parameters for your individual test."""
        self.setup_clean_chain = False
        self.num_nodes = 1
        self.extra_args = [[]]

    def run_test(self):
        CALLSTRING_CHAR_COUNT = 64

        self.nodes[0].generate(COINBASE_MATURITY + 50)

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
                                 "0" * (CALLSTRING_CHAR_COUNT - len(format(13, 'x'))) + format(13, 'x'))  # Vote duration

        # 4. Create vote
        self.nodes[0].sendtocontract("0000000000000000000000000000000000000091", createvote_callstring, 0, 2500000, main_address)

        self.nodes[0].generate(1)

        vote_for_callstring = "4b9f5c98" + "000000000000000000000000000000000000000000000000000000000000000" + "1" # vote for
        vote_against_callstring = "4b9f5c98" + "000000000000000000000000000000000000000000000000000000000000000" + "0" # vote against

        self.nodes[0].sendtocontract("0000000000000000000000000000000000000091", vote_for_callstring, 0.00000001, 2500000, main_address)
        self.nodes[0].sendtocontract("0000000000000000000000000000000000000091", vote_against_callstring, 0.00000001, 2500000, main_address)

        self.nodes[0].generate(1)

        votesfor_callstring = "c94d6b17"
        votesagainst_callstring = "7d007f49"
        votesfor_ret = self.nodes[0].callcontract("0000000000000000000000000000000000000091", votesfor_callstring)
        votesagainst_ret = self.nodes[0].callcontract("0000000000000000000000000000000000000091", votesagainst_callstring)

        assert_equal(int(votesfor_ret['executionResult']['output'], 16), 1)
        assert_equal(int(votesagainst_ret['executionResult']['output'], 16), 1)
        #check ret

if __name__ == '__main__':
    DgpTest().main()
