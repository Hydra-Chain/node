// Copyright (c) 2018 LockTrip
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <utilstrencodings.h>
#include "dgp.h"
#include "qtum/qtumstate.h"
#include "libethcore/ABI.h"
#include "util.h"
#include "price-oracle.h"

PriceOracle::PriceOracle() {
    this->m_contractAbi.loads(PRICE_ORACLE_CONTRACT_ABI);
}

bool PriceOracle::getPrice(uint64_t &gasPrice) {
    std::string callString {};
    std::vector<std::vector<std::string>> values{};
    bool status = this->generateCallString(values, callString, GET_PRICE);

    if (status) {
        std::vector<ResultExecute> result = CallContract(LockTripPriceOracleContract, ParseHex(callString));

        if (!result.empty()) {
            dev::bytesConstRef o(&result[0].execRes.output);
            dev::u256 data = dev::eth::ABIDeserialiser<dev::u256>::deserialise(o);
            gasPrice = uint64_t(dev::u256(dev::h256(data)));

            if(gasPrice == 0){
                gasPrice = DEFAULT_GAS_PRICE;
            }

            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}

bool PriceOracle::getBytePrice(uint64_t& bytePrice) {
    std::string callString {};
    std::vector<std::vector<std::string>> values{};
    bool status = this->generateCallString(values, callString, GET_BYTE_PRICE);

    if (status) {
        std::vector<ResultExecute> result = CallContract(LockTripPriceOracleContract, ParseHex(callString));

        if (!result.empty()) {
            dev::bytesConstRef o(&result[0].execRes.output);
            dev::u256 data = dev::eth::ABIDeserialiser<dev::u256>::deserialise(o);
            bytePrice = uint64_t(dev::u256(dev::h256(data)));

            if(bytePrice == 0){
                bytePrice = DEFAULT_BYTE_PRICE;
            }

            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}