// Copyright (c) 2018 LockTrip
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/strencodings.h>
#include <sstream>
#include "dgp.h"
#include "qtum/qtumstate.h"
#include "libethcore/ABI.h"
#include <util/system.h>
#include <logging.h>

Dgp::Dgp() {
    this->m_contractAbi.loads(DGP_CONTRACT_ABI);
}

bool Dgp::hasVoteInProgress(bool& voteInProgress) {
    LOCK(cs_main);
    std::string callString {};
    std::vector<std::vector<std::string>> values{};
    bool status = this->generateCallString(values, callString, HAS_VOTE_IN_PROGRESS);

    if (status) {
        std::vector<ResultExecute> result = CallContract(LockTripDgpContract, ParseHex(callString), dev::Address(), 0, DEFAULT_BLOCK_GAS_LIMIT_DGP);

        if (!result.empty()) {
            dev::bytesConstRef o(&result[0].execRes.output);
            dev::u256 outData = dev::eth::ABIDeserialiser<dev::u256>::deserialise(o);
            voteInProgress = outData == 0 ? false : true;

            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}

bool Dgp::getCurrentVote(dgp_currentVote& currentVote) {
    return this->fillCurrentVoteAddressInfo(CURRENT_VOTE_NEWADMIN, currentVote.newAdmin) &&
            this->fillCurrentVoteAddressInfo(CURRENT_VOTE_CREATOR, currentVote.vote_creator) &&
            this->fillCurrentVoteUintInfo(CURRENT_VOTE_VOTESFOR, currentVote.votesFor) &&
            this->fillCurrentVoteUintInfo(CURRENT_VOTE_VOTESAGAINST, currentVote.votesAgainst) &&
            this->fillCurrentVoteUintInfo(CURRENT_VOTE_STARTBLOCK, currentVote.start_block) &&
            this->fillCurrentVoteUintInfo(CURRENT_VOTE_BLOCKSEXPIRATION, currentVote.blocksExpiration) &&
            this->fillCurrentVoteUintInfo(CURRENT_VOTE_PARAM, currentVote.param) &&
            this->fillCurrentVoteUintInfo(CURRENT_VOTE_VALUE, currentVote.param_value) &&
            this->fillCurrentVoteUintInfo(CURRENT_VOTE_THRESHOLD, currentVote.threshold);
}

bool Dgp::fillCurrentVoteUintInfo(dgp_contract_funcs func, uint64_t& container) {
    LOCK(cs_main);
    std::string callString {};
    std::vector<std::vector<std::string>> values{};
    bool status = this->generateCallString(values, callString, func);
    if (status) {
        std::vector<ResultExecute> result = CallContract(LockTripDgpContract, ParseHex(callString), dev::Address(), 0, DEFAULT_BLOCK_GAS_LIMIT_DGP);

        if (!result.empty()) {
            dev::bytesConstRef o(&result[0].execRes.output);
            dev::u256 outData = dev::eth::ABIDeserialiser<dev::u256>::deserialise(o);
            container = uint64_t(dev::u256(dev::h256(outData)));

            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}

bool Dgp::fillCurrentVoteAddressInfo(dgp_contract_funcs func, dev::Address& container) {
    LOCK(cs_main);
    std::string callString {};
    std::vector<std::vector<std::string>> values{};
    bool status = this->generateCallString(values, callString, func);
    if (status) {
        std::vector<ResultExecute> result = CallContract(LockTripDgpContract, ParseHex(callString), dev::Address(), 0, DEFAULT_BLOCK_GAS_LIMIT_DGP);

        if (!result.empty()) {
            dev::bytesConstRef o(&result[0].execRes.output);
            dev::Address outData = dev::eth::ABIDeserialiser<dev::Address>::deserialise(o);
            container = outData;

            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}

bool Dgp::isParamVoted(dgp_params param, bool& isVoted) {
    LOCK(cs_main);
    std::vector<std::string> params {std::to_string(param)};
    std::string callString {};
    std::vector<std::vector<std::string>> values{params};
    bool status = this->generateCallString(values, callString, PARAM_VOTED);
    if (status) {
        std::vector<ResultExecute> result = CallContract(LockTripDgpContract, ParseHex(callString), dev::Address(), 0, DEFAULT_BLOCK_GAS_LIMIT_DGP);

        if (!result.empty()) {
            dev::bytesConstRef o(&result[0].execRes.output);
            dev::u256 outData = dev::eth::ABIDeserialiser<dev::u256>::deserialise(o);
            isVoted = outData == 0 ? false : true;

            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}

bool Dgp::getVoteBlockExpiration(uint64_t& expiration) {
    LOCK(cs_main);
    std::string callString {};
    std::vector<std::vector<std::string>> values{};
    bool status = this->generateCallString(values, callString, GET_VOTE_EXPIRATION);

    if (status) {
        std::vector<ResultExecute> result = CallContract(LockTripDgpContract, ParseHex(callString), dev::Address(), 0, DEFAULT_BLOCK_GAS_LIMIT_DGP);

        if (!result.empty()) {
            dev::bytesConstRef o(&result[0].execRes.output);
            dev::u256 data = dev::eth::ABIDeserialiser<dev::u256>::deserialise(o);
            expiration = uint64_t(dev::u256(dev::h256(data)));
            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}

bool Dgp::getDgpParam(dgp_params param, uint64_t& value) {
    LOCK(cs_main);
    std::vector<std::string> params {std::to_string(param)};
    std::string callString {};
    std::vector<std::vector<std::string>> values{params};
    bool status = this->generateCallString(values, callString, GET_DGP_PARAM);

    if (status) {
        std::vector<ResultExecute> result = CallContract(LockTripDgpContract, ParseHex(callString), dev::Address(), 0, DEFAULT_BLOCK_GAS_LIMIT_DGP);

        if (!result.empty()) {
            dev::bytesConstRef o(&result[0].execRes.output);
            dev::u256 data = dev::eth::ABIDeserialiser<dev::u256>::deserialise(o);
            value = uint64_t(dev::u256(dev::h256(data)));
            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}

bool Dgp::convertFiatThresholdToLoc(uint64_t& fiatThresholdInCents, uint64_t& locContainer) {
    LOCK(cs_main);
    std::vector<std::string> params {std::to_string(fiatThresholdInCents * ONE_CENT_EQUAL)};
    std::string callString {};
    std::vector<std::vector<std::string>> values{params};
    bool status = this->generateCallString(values, callString, CONVERT_FIAT_THRESHOLD_TO_LOC);

    if (status) {
        std::vector<ResultExecute> result = CallContract(LockTripDgpContract, ParseHex(callString), dev::Address(), 0, DEFAULT_BLOCK_GAS_LIMIT_DGP);

        if (!result.empty()) {
            dev::bytesConstRef o(&result[0].execRes.output);
            dev::u256 data = dev::eth::ABIDeserialiser<dev::u256>::deserialise(o);
            locContainer = uint64_t(dev::u256(dev::h256(data)));
            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}

bool Dgp::finishVote(CScript& scriptPubKey) {
    scriptPubKey = CScript();

    std::string callString{};
    std::vector<std::vector<std::string>> values{};

    bool status = this->generateCallString(values, callString, FINISH_VOTE);

    if (status) {
        scriptPubKey << CScriptNum(VersionVM::GetEVMDefault().toRaw()) <<  ParseHex(callString)
                     << LockTripDgpContract.asBytes() << OP_COINSTAKE_CALL;
        return true;
    } else {
        return false;
    }
}

void Dgp::calculateGasPriceBuffer(CAmount gasPrice, CAmount& gasPriceBuffer) {
    gasPriceBuffer = gasPrice / 5 + gasPrice % 5;
}

void Dgp::updateDgpCache() {
    this->updateDgpCacheParam(FIAT_GAS_PRICE, DGP_CACHE_FIAT_GAS_PRICE);
    if(DGP_CACHE_FIAT_GAS_PRICE < DEFAULT_MIN_GAS_PRICE_DGP)
        DGP_CACHE_FIAT_GAS_PRICE = DEFAULT_MIN_GAS_PRICE_DGP;

    this->updateDgpCacheParam(BURN_RATE, DGP_CACHE_BURN_RATE);
    if(DGP_CACHE_BURN_RATE < MIN_BURN_RATE_PERCENTAGE ||
            DGP_CACHE_BURN_RATE > MAX_BURN_RATE_PERCENTAGE)
        DGP_CACHE_BURN_RATE = DEFAULT_BURN_RATE_PERCENTAGE;

    this->updateDgpCacheParam(ECONOMY_DIVIDEND, DGP_CACHE_ECONOMY_DIVIDEND);
    if(DGP_CACHE_ECONOMY_DIVIDEND < MIN_ECONOMY_DIVIDEND_PERCENTAGE ||
            DGP_CACHE_ECONOMY_DIVIDEND > MAX_ECONOMY_DIVIDEND_PERCENTAGE)
        DGP_CACHE_ECONOMY_DIVIDEND = DEFAULT_ECONOMY_DIVIDEND_PERCENTAGE;

    this->updateDgpCacheParam(BLOCK_SIZE_DGP_PARAM, DGP_CACHE_BLOCK_SIZE);
    if(DGP_CACHE_BLOCK_SIZE < MIN_BLOCK_SIZE_DGP ||
            DGP_CACHE_BLOCK_SIZE > MAX_BLOCK_SIZE_DGP)
        DGP_CACHE_BLOCK_SIZE = DEFAULT_BLOCK_SIZE_DGP;

    this->updateDgpCacheParam(BLOCK_GAS_LIMIT_DGP_PARAM, DGP_CACHE_BLOCK_GAS_LIMIT);
    if(DGP_CACHE_BLOCK_GAS_LIMIT < MIN_BLOCK_GAS_LIMIT_DGP ||
            DGP_CACHE_BLOCK_GAS_LIMIT > MAX_BLOCK_GAS_LIMIT_DGP)
        DGP_CACHE_BLOCK_GAS_LIMIT = DEFAULT_BLOCK_GAS_LIMIT_DGP;

    this->updateDgpCacheParam(FIAT_BYTE_PRICE, DGP_CACHE_FIAT_BYTE_PRICE);
    if(DGP_CACHE_FIAT_BYTE_PRICE < DEFAULT_MIN_BYTE_PRICE_DGP)
        DGP_CACHE_FIAT_BYTE_PRICE = DEFAULT_MIN_BYTE_PRICE_DGP;
}

void Dgp::updateDgpCacheParam(dgp_params param, uint64_t& cache) {
    LOCK(cs_main);
    bool isParamVoted = false;

    this->isParamVoted(param, isParamVoted);

    if (isParamVoted) {
        this->getDgpParam(param, cache);
    }
}

bool Dgp::fillBlockRewardBlocksInfo() {
    LOCK(cs_main);
    std::string callString {};
    std::vector<std::vector<std::string>> values{};
    bool status = this->generateCallString(values, callString, GET_BLOCK_REWARD_VOTE_BLOCKS);

    if (status) {
        std::vector<ResultExecute> result = CallContract(LockTripDgpContract, ParseHex(callString), dev::Address(), 0, DEFAULT_BLOCK_GAS_LIMIT_DGP);
        if (!result.empty()) {
            std::string output = HexStr(result[0].execRes.output);
            this->blockRewardVoteBlocks.clear();
            for(int i = 128; i < output.length(); i+=64) {
                std::string current = output.substr(i, 64);
                uint64_t num = uint64_t(dev::u256(dev::h256(current)));
                this->blockRewardVoteBlocks.push_back(num);
            }

            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}

bool Dgp::fillBlockRewardPercentageInfo() {
    LOCK(cs_main);
    std::string callString {};
    std::vector<std::vector<std::string>> values{};
    bool status = this->generateCallString(values, callString, GET_BLOCK_REWARD_VOTE_PERCENTAGES);

    if (status) {
        std::vector<ResultExecute> result = CallContract(LockTripDgpContract, ParseHex(callString), dev::Address(), 0, DEFAULT_BLOCK_GAS_LIMIT_DGP);

        if (!result.empty()) {
            std::string output = HexStr(result[0].execRes.output);
            this->blockRewardVotePercentages.clear();
            for(int i = 128; i < output.length(); i+=64) {
                std::string current = output.substr(i, 64);
                uint64_t num = uint64_t(dev::u256(dev::h256(current)));
                this->blockRewardVotePercentages.push_back(num);
            }

            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}