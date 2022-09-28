// Copyright (c) 2018 LockTrip
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef LOCKTRIP_CONTRACT_PROXY_H
#define LOCKTRIP_CONTRACT_PROXY_H

#include <string>
#include <map>
#include <cpp-ethereum/libdevcrypto/Common.h>
#include "util/contractabi.h"
#include "validation.h"

class ContractProxy {
public:
    ContractProxy();
    std::string getContractFunctionHex(int func) const;

protected:
    bool generateCallString(std::vector<std::vector<std::string>>& values, std::string& callString, std::uint8_t funcId) const;

public:
    ContractABI m_contractAbi;
};

#endif //LOCKTRIP_CONTRACT_PROXY_H
