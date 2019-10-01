// Copyright (c) 2018 LockTrip
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/strencodings.h>
#include "economy.h"
#include "qtum/qtumstate.h"
#include <util/system.h>

#define CONTRACTOWNER_ADDR_LEN  40

Economy::Economy() {
    this->m_contractAbi.loads(ECONOMY_CONTRACT_ABI);
}

bool Economy::getContractOwner(const dev::Address contract, dev::Address &owner) const {
    std::vector<std::string> params{contract.hex()};
    std::string callString{};
    std::vector<std::vector<std::string>> values{params};
    bool status = this->generateCallString(values, callString, GET_OWNER_FUNC_ID);

    if (status) {
        std::vector<ResultExecute> result = CallContract(LockTripEconomyContract, ParseHex(callString));

        if (!result.empty()) {
            std::string res = HexStr(result[0].execRes.output);
            dev::Address contractOwner(res.substr(res.length() - CONTRACTOWNER_ADDR_LEN));
            owner = contractOwner;
            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}

bool Economy::getCScriptForAddContract(std::vector<dev::Address> &contractAddresses, std::vector<dev::Address> &ownerAddresses,
                              CScript &scriptPubKey) {
    scriptPubKey = CScript();

    if (contractAddresses.size() == ownerAddresses.size()) {
        std::vector<std::vector<std::string>> params{std::vector<std::string>{}, std::vector<std::string>{}};

        for (auto i = 0; i < contractAddresses.size(); i++) {
            dev::Address contractAddress = contractAddresses[i];
            dev::Address ownerAddress = ownerAddresses[i];
            params[0].push_back(contractAddress.hex());
            params[1].push_back(ownerAddress.hex());
        }

        std::string callString{};
        bool status = this->generateCallString(params, callString, ADD_CONTRACT_FUNC_ID);
        if (status) {
            scriptPubKey << CScriptNum(VersionVM::GetEVMDefault().toRaw()) <<  ParseHex(callString)
                         << LockTripEconomyContract.asBytes() << OP_COINSTAKE_CALL;
            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}