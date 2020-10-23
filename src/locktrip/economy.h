// Copyright (c) 2018 LockTrip
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef LOCKTRIP_ECONOMY_H
#define LOCKTRIP_ECONOMY_H

#include <string>
#include <map>
#include <cpp-ethereum/libdevcrypto/Common.h>
#include "contractabi-base.h"
#include "validation.h"
#include "contract-proxy.h"

static const dev::Address LockTripEconomyContract = dev::Address("0000000000000000000000000000000000000090");

//static const std::string GET_CONTRACT_OWNER_ABI = "6d55b6e9";

static const std::string ECONOMY_CONTRACT_ABI = "[\n"
                                                "\t{\n"
                                                "\t\t\"constant\": true,\n"
                                                "\t\t\"inputs\": [\n"
                                                "\t\t\t{\n"
                                                "\t\t\t\t\"name\": \"\",\n"
                                                "\t\t\t\t\"type\": \"address\"\n"
                                                "\t\t\t}\n"
                                                "\t\t],\n"
                                                "\t\t\"name\": \"contractOwners\",\n"
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
                                                "\t\t\t\t\"name\": \"_contractAddresses\",\n"
                                                "\t\t\t\t\"type\": \"address[]\"\n"
                                                "\t\t\t},\n"
                                                "\t\t\t{\n"
                                                "\t\t\t\t\"name\": \"_ownerAddresses\",\n"
                                                "\t\t\t\t\"type\": \"address[]\"\n"
                                                "\t\t\t}\n"
                                                "\t\t],\n"
                                                "\t\t\"name\": \"addContract\",\n"
                                                "\t\t\"outputs\": [],\n"
                                                "\t\t\"payable\": false,\n"
                                                "\t\t\"stateMutability\": \"nonpayable\",\n"
                                                "\t\t\"type\": \"function\"\n"
                                                "\t},\n"
                                                "\t{\n"
                                                "\t\t\"constant\": false,\n"
                                                "\t\t\"inputs\": [\n"
                                                "\t\t\t{\n"
                                                "\t\t\t\t\"name\": \"_contractAddress\",\n"
                                                "\t\t\t\t\"type\": \"address\"\n"
                                                "\t\t\t},\n"
                                                "\t\t\t{\n"
                                                "\t\t\t\t\"name\": \"_ownerAddress\",\n"
                                                "\t\t\t\t\"type\": \"address\"\n"
                                                "\t\t\t}\n"
                                                "\t\t],\n"
                                                "\t\t\"name\": \"updateContract\",\n"
                                                "\t\t\"outputs\": [],\n"
                                                "\t\t\"payable\": false,\n"
                                                "\t\t\"stateMutability\": \"nonpayable\",\n"
                                                "\t\t\"type\": \"function\"\n"
                                                "\t}\n"
                                                "]";

enum economy_contract_funcs {
    GET_OWNER_FUNC_ID = 0,
    ADD_CONTRACT_FUNC_ID = 1,
    UPDATE_CONTRACT_FUNC_ID = 2
};

class Economy : public ContractProxy {

public:
    Economy();
    bool getContractOwner(dev::Address contract, dev::Address& owner) const;
    bool getCScriptForAddContract(std::vector<dev::Address>& contractAddresses, std::vector<dev::Address>& ownerAddresses,
            CScript& scriptPubKey);

};

#endif //LOCKTRIP_ECONOMY_H
