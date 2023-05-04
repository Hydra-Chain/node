// Copyright (c) 2022 HYDRA
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/strencodings.h>
#include "lydra.h"
#include "qtum/qtumstate.h"
#include "libethcore/ABI.h"
#include <util/system.h>

Lydra::Lydra() {
    this->m_contractAbi.loads(LYDRA_CONTRACT_ABI);
}

bool Lydra::getMintDatahex(std::string& datahex) {
    datahex = this->getContractFunctionHex(MINT);
    return true;
}

bool Lydra::getBurnDatahex(std::string& datahex, int64_t amount) {
    if(amount == -1) {
        datahex = this->getContractFunctionHex(BURN_ALL);
        return true;
    } else {
        std::vector<std::string> params {std::to_string(amount)};
        std::vector<std::vector<std::string>> values {params};
        this->generateCallString(values, datahex, BURN);
        return true;
    }
}

bool Lydra::getLockedHydraAmountPerAddress(dev::Address lydraContract, std::string lydraAddress, uint64_t& amount) {
    std::vector<std::string> params{lydraAddress};
    std::string callString{};
    std::vector<std::vector<std::string>> values{params};
    bool status = this->generateCallString(values, callString, LOCKED_BALANCE);

    if (status) {
        std::vector<ResultExecute> result = CallContract(lydraContract, ParseHex(callString));

        if (!result.empty()) {
            dev::bytesConstRef o(&result[0].execRes.output);
            dev::u256 data = dev::eth::ABIDeserialiser<dev::u256>::deserialise(o);
            amount = uint64_t(dev::u256(dev::h256(data)));
            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}