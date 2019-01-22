// Copyright (c) 2018 LockTrip
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef LOCKTRIP_DGP_H
#define LOCKTRIP_DGP_H

#include <string>
#include <map>

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

static const uint32_t MIN_BLOCK_SIZE_DGP = 500000;
static const uint32_t MAX_BLOCK_SIZE_DGP = 32000000;
static const uint32_t DEFAULT_BLOCK_SIZE_DGP = 2000000;

static const uint64_t DEFAULT_MIN_GAS_PRICE_DGP = 1;

static const uint64_t DEFAULT_MIN_BYTE_PRICE_DGP = 1;

static const uint64_t MIN_BLOCK_GAS_LIMIT_DGP = 1000000;
static const uint64_t MAX_BLOCK_GAS_LIMIT_DGP = 1000000000;
static const uint64_t DEFAULT_BLOCK_GAS_LIMIT_DGP = 40000000;

static const uint64_t ONE_CENT_EQUAL = 1000000; // representing fiat money like LOC and satoshi

// Array indexes are the same as the DGP param IDs
const auto VOTE_HEADLINES = {
    "Vote for adding new admin",
    "Vote for removing admin",
    "Vote for fiat gas price change",
    "Vote for burn rate % change",
    "Vote for economy dividend % change",
    "Vote for block size change",
    "Vote for block gas limit change",
    "Vote for fiat transaction byte price change"
};

static const dev::Address LockTripDgpContract = dev::Address("0000000000000000000000000000000000000091");

static const std::string DGP_CONTRACT_ABI = "";

enum dgp_params {
    ADMIN_VOTE = 0,
    REMOVE_ADMIN_VOTE = 1,
    FIAT_GAS_PRICE = 2,
    BURN_RATE = 3,
    ECONOMY_DIVIDEND = 4,
    BLOCK_SIZE_DGP_PARAM = 5,
    BLOCK_GAS_LIMIT_DGP_PARAM = 6,
    FIAT_BYTE_PRICE = 7
};

enum dgp_contract_funcs {
    FINISH_VOTE = 1,
    GET_DGP_PARAM = 2,
    HAS_VOTE_IN_PROGRESS = 18,
    GET_VOTE_EXPIRATION = 12,
    PARAM_VOTED = 15,
    VOTE = 7,
    CONVERT_FIAT_THRESHOLD_TO_LOC = 23,
    ///////////////
    CURRENT_VOTE_NEWADMIN = 6,
    CURRENT_VOTE_VOTESFOR = 26,
    CURRENT_VOTE_VOTESAGAINST = 19,
    CURRENT_VOTE_STARTBLOCK = 8,
    CURRENT_VOTE_BLOCKSEXPIRATION = 9,
    CURRENT_VOTE_PARAM = 10,
    CURRENT_VOTE_VALUE = 27,
    CURRENT_VOTE_CREATOR = 22,
    CURRENT_VOTE_THRESHOLD = 13
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
};

#endif //LOCKTRIP_DGP_H
