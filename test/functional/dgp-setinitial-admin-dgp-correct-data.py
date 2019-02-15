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
        main_address = self.nodes[0].gethexaddress(self.nodes[0].getnewaddress())
        backup_address = self.nodes[0].gethexaddress(self.nodes[0].getnewaddress())
        callstring = "7fd05e2a" + "000000000000000000000000" + main_address + "000000000000000000000000" + backup_address

        self.nodes[0].sendtocontract("0000000000000000000000000000000000000091", callstring)

        self.nodes[0].generate(1)

        get_callstring = "2fc78e4c000000000000000000000000000000000000000000000000000000000000000"
        ret = self.nodes[0].callcontract("0000000000000000000000000000000000000091", get_callstring + "2")

        assert_equal(int(ret['executionResult']['output'], 16), 1000)
        #check ret

        ret = self.nodes[0].callcontract("0000000000000000000000000000000000000091", get_callstring + "7")

        assert_equal(int(ret['executionResult']['output'], 16), 1000)
        #check ret

        ret = self.nodes[0].callcontract("0000000000000000000000000000000000000091", get_callstring + "8")

        assert_equal(int(ret['executionResult']['output'], 16), 32)

if __name__ == '__main__':
    DgpTest().main()
