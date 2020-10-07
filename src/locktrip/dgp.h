// Copyright (c) 2018 LockTrip
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef LOCKTRIP_DGP_H
#define LOCKTRIP_DGP_H

#include <string>
#include <map>
#include <vector>

#include <cpp-ethereum/libdevcrypto/Common.h>
#include "contractabi-base.h"
#include "validation.h"
#include "contract-proxy.h"

static const uint64_t DEFAULT_BURN_RATE_PERCENTAGE = 0;
static const uint64_t MIN_BURN_RATE_PERCENTAGE = 0;
static const uint64_t MAX_BURN_RATE_PERCENTAGE = 50;

static const uint64_t DEFAULT_ECONOMY_DIVIDEND_PERCENTAGE = 50;
static const uint64_t MIN_ECONOMY_DIVIDEND_PERCENTAGE = 0;
static const uint64_t MAX_ECONOMY_DIVIDEND_PERCENTAGE = 50;

static const uint64_t MIN_BLOCK_TIME = 32;
static const uint64_t MAX_BLOCK_TIME = 1200;

static const uint32_t MIN_BLOCK_SIZE_DGP = 500000;
static const uint32_t MAX_BLOCK_SIZE_DGP = 32000000;
static const uint32_t DEFAULT_BLOCK_SIZE_DGP = 2000000;

static const uint64_t DEFAULT_MIN_GAS_PRICE_DGP = 1;

static const uint64_t DEFAULT_MIN_BYTE_PRICE_DGP = 1;

static const uint64_t DEFAULT_BLOCK_TIME = 32;

static const uint64_t MIN_BLOCK_GAS_LIMIT_DGP = 1000000;
static const uint64_t MAX_BLOCK_GAS_LIMIT_DGP = 1000000000;
static const uint64_t DEFAULT_BLOCK_GAS_LIMIT_DGP = 40000000;

static const uint64_t MIN_BLOCK_REWARD_PERCENTAGE_DGP = 0;
static const uint64_t MAX_BLOCK_REWARD_PERCENTAGE_DGP = 25;
static const uint64_t DEFAULT_BLOCK_REWARD_PERCENTAGE_DGP = 25;

static const uint64_t ONE_CENT_EQUAL = 1000000; // representing fiat money like LOC and satoshi

// DGP CACHE GLOBALS

static uint64_t DGP_CACHE_FIAT_GAS_PRICE = 1000;
static uint64_t DGP_CACHE_BURN_RATE = DEFAULT_BURN_RATE_PERCENTAGE;
static uint64_t DGP_CACHE_ECONOMY_DIVIDEND = DEFAULT_ECONOMY_DIVIDEND_PERCENTAGE;
static uint64_t DGP_CACHE_BLOCK_SIZE = DEFAULT_BLOCK_SIZE_DGP;
static uint64_t DGP_CACHE_BLOCK_GAS_LIMIT = DEFAULT_BLOCK_GAS_LIMIT_DGP;
static uint64_t DGP_CACHE_FIAT_BYTE_PRICE = 1000;
static uint64_t DGP_CACHE_BLOCK_REWARD_PERCENTAGE = DEFAULT_BLOCK_REWARD_PERCENTAGE_DGP;

// Array indexes are the same as the DGP param IDs
const auto VOTE_HEADLINES = {
    "Vote for adding new admin",
    "Vote for removing admin",
    "Vote for fiat gas price change",
    "Vote for burn rate % change",
    "Vote for economy reimbursement % change",
    "Vote for block size change",
    "Vote for block gas limit change",
    "Vote for fiat transaction byte price change",
    "Vote for block reward % change"
};

static const dev::Address LockTripDgpContract = dev::Address("0000000000000000000000000000000000000091");

static const std::string DGP_CONTRACT_ABI = "[\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"VOTING_TIMEOUT\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": false,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"finishVote\",\n"
                                            "\t\t\"outputs\": [],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"nonpayable\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"param\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"name\": \"getDgpParams\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"isOracleSet\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"bool\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"address\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"name\": \"timeoutForAdmin\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"EQUAL_VOTES_EXTENSION\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"address\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"name\": \"admins\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"bool\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"getCurrentVoteAdminAddress\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"address\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": false,\n"
                                            "\t\t\"inputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"value\",\n"
                                            "\t\t\t\t\"type\": \"bool\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"name\": \"vote\",\n"
                                            "\t\t\"outputs\": [],\n"
                                            "\t\t\"payable\": true,\n"
                                            "\t\t\"stateMutability\": \"payable\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"getCurrentVoteStartBlock\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"getCurrentVoteBlocksExpiration\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"getCurrentVoteParam\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"ONE_CENT_EQUAL\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"getVoteExpiration\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"getCurrentVoteTreshold\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"REMOVE_ADMIN_VOTE_TRESHOLD\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"param\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"name\": \"isParamVoted\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"bool\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": false,\n"
                                            "\t\t\"inputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"param\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t},\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"newValue\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t},\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"blocksExpiration\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"name\": \"createVote\",\n"
                                            "\t\t\"outputs\": [],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"nonpayable\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"STANDARD_VOTE_TRESHOLD\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"hasVoteInProgress\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"bool\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": false,\n"
                                            "\t\t\"inputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"a\",\n"
                                            "\t\t\t\t\"type\": \"address\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"name\": \"setOracle\",\n"
                                            "\t\t\"outputs\": [],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"nonpayable\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"getCurrentVoteVotesAgainst\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"oracle\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"address\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": false,\n"
                                            "\t\t\"inputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"admin\",\n"
                                            "\t\t\t\t\"type\": \"address\"\n"
                                            "\t\t\t},\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"backupAddress\",\n"
                                            "\t\t\t\t\"type\": \"address\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"name\": \"setInitialAdmin\",\n"
                                            "\t\t\"outputs\": [],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"nonpayable\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"getCurrentVoteCreator\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"address\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"fiatThresholdInCents\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"name\": \"convertFiatVoteThresholdToLoc\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": false,\n"
                                            "\t\t\"inputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"addressToVoteFor\",\n"
                                            "\t\t\t\t\"type\": \"address\"\n"
                                            "\t\t\t},\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"blocksExpiration\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t},\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"param\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"name\": \"createAdminVote\",\n"
                                            "\t\t\"outputs\": [],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"nonpayable\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"name\": \"dgp_paramsPerAddress\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"getCurrentVoteVotesFor\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"getCurrentVoteParamValue\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"name\": \"dgp_paramVoted\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"bool\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"voteInProgress\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"bool\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"isInitialAdminSet\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"\",\n"
                                            "\t\t\t\t\"type\": \"bool\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t},\n"
                                            "\t{\n"
                                            "\t\t\"constant\": true,\n"
                                            "\t\t\"inputs\": [],\n"
                                            "\t\t\"name\": \"currentVote\",\n"
                                            "\t\t\"outputs\": [\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"votesFor\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t},\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"votesAgainst\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t},\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"start_block\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t},\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"blocksExpiration\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t},\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"param\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t},\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"param_value\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t},\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"treshold\",\n"
                                            "\t\t\t\t\"type\": \"uint256\"\n"
                                            "\t\t\t},\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"adminAddress\",\n"
                                            "\t\t\t\t\"type\": \"address\"\n"
                                            "\t\t\t},\n"
                                            "\t\t\t{\n"
                                            "\t\t\t\t\"name\": \"vote_creator\",\n"
                                            "\t\t\t\t\"type\": \"address\"\n"
                                            "\t\t\t}\n"
                                            "\t\t],\n"
                                            "\t\t\"payable\": false,\n"
                                            "\t\t\"stateMutability\": \"view\",\n"
                                            "\t\t\"type\": \"function\"\n"
                                            "\t}\n"
                                            "]";

enum dgp_params {
    ADMIN_VOTE = 0,
    REMOVE_ADMIN_VOTE = 1,
    FIAT_GAS_PRICE = 2,
    BURN_RATE = 3,
    ECONOMY_DIVIDEND = 4,
    BLOCK_SIZE_DGP_PARAM = 5,
    BLOCK_GAS_LIMIT_DGP_PARAM = 6,
    FIAT_BYTE_PRICE = 7,
    BLOCK_REWARD_PERCENTAGE = 8
};

enum dgp_contract_funcs {
    FINISH_VOTE = 1,
    GET_DGP_PARAM = 2,
    HAS_VOTE_IN_PROGRESS = 19,
    GET_VOTE_EXPIRATION = 13,
    PARAM_VOTED = 16,
    VOTE = 8,
    CONVERT_FIAT_THRESHOLD_TO_LOC = 25,
    ///////////////
    CURRENT_VOTE_NEWADMIN = 7,
    CURRENT_VOTE_VOTESFOR = 28,
    CURRENT_VOTE_VOTESAGAINST = 21,
    CURRENT_VOTE_STARTBLOCK = 9,
    CURRENT_VOTE_BLOCKSEXPIRATION = 10,
    CURRENT_VOTE_PARAM = 11,
    CURRENT_VOTE_VALUE = 29,
    CURRENT_VOTE_CREATOR = 24,
    CURRENT_VOTE_THRESHOLD = 14
};

struct dgp_currentVote {
    uint64_t votesFor;
    uint64_t votesAgainst;
    uint64_t start_block;
    uint64_t blocksExpiration;
    uint64_t param;
    uint64_t param_value;
    uint64_t threshold;
    dev::Address newAdmin;
    dev::Address vote_creator;
};

class Dgp : public ContractProxy {

public:
    Dgp();
    bool hasVoteInProgress(bool& voteInProgress);
    bool getVoteBlockExpiration(uint64_t& expiration);
    bool finishVote(CScript& scriptPubKey);
    bool getDgpParam(dgp_params param, uint64_t& value);
    bool isParamVoted(dgp_params param, bool& isVoted);
    bool getCurrentVote(dgp_currentVote& currentVote);
    bool fillCurrentVoteUintInfo(dgp_contract_funcs func, uint64_t& container);
    bool fillCurrentVoteAddressInfo(dgp_contract_funcs func, dev::Address& container);
    void calculateGasPriceBuffer(CAmount gasPrice, CAmount& gasPriceBuffer);
    bool convertFiatThresholdToLoc(uint64_t& fiatThresholdInCents, uint64_t& locContainer);
    void updateDgpCache();
    bool fillBlockRewardBlocksInfo();
    bool fillBlockRewardPercentageInfo();

    std::vector<uint64_t> blockRewardVoteBlocks;
    std::vector<uint8_t> blockRewardVotePercentages;

private:
    void updateDgpCacheParam(dgp_params param, uint64_t& cache);
};

#endif //LOCKTRIP_DGP_H
