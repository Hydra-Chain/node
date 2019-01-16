// Copyright (c) 2018 LockTrip
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <utilstrencodings.h>
#include <sstream>
#include "dgp.h"
#include "qtum/qtumstate.h"
#include "libethcore/ABI.h"
#include "util.h"

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
            this->fillCurrentVoteUintInfo(CURRENT_VOTE_VALUE, currentVote.param_value);
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