// Copyright (c) 2018 LockTrip
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/strencodings.h>
#include "contract-proxy.h"
#include "qtum/qtumstate.h"
#include <util/system.h>

ContractProxy::ContractProxy() {}

std::string ContractProxy::getContractFunctionHex(int func) const {
    FunctionABI function = this->m_contractAbi.functions[func];

    return function.selector();
}

bool ContractProxy::generateCallString(std::vector<std::vector<std::string>> &values, std::string &callString,
                                 const std::uint8_t funcId) const {
    FunctionABI function = this->m_contractAbi.functions[funcId];
    std::vector<ParameterABI::ErrorType> errors;

    return function.abiIn(values, callString, errors);
}