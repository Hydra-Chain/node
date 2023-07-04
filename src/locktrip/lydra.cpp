// Copyright (c) 2022 HYDRA
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "lydra.h"
#include "libethcore/ABI.h"
#include "qtum/qtumstate.h"
#include <util/strencodings.h>
#include <util/system.h>
#include <wallet/wallet.h>
#include <interfaces/wallet.h>
#include <chainparams.h>

void updateLydraLockedCache(int64_t& amount, std::string address, bool isMinting)
{
    if(LYDRA_LOCKED_CACHE_AMOUNT_PER_ADDRESS.count(address)){
        if (isMinting) {
            LYDRA_LOCKED_CACHE_AMOUNT_PER_ADDRESS[address] += amount;
        } else {
            if (amount >= LYDRA_LOCKED_CACHE_AMOUNT_PER_ADDRESS[address]) clearLydraLockedCache(address);
            else LYDRA_LOCKED_CACHE_AMOUNT_PER_ADDRESS[address] -= amount;
        }
    } else {
        if (isMinting) {
            LYDRA_LOCKED_CACHE_AMOUNT_PER_ADDRESS.insert({address, amount});
        }
    }
}

void clearLydraLockedCache(std::string address)
{
    if(LYDRA_LOCKED_CACHE_AMOUNT_PER_ADDRESS.count(address)){
        LYDRA_LOCKED_CACHE_AMOUNT_PER_ADDRESS[address] = 0;
    } else {
        LYDRA_LOCKED_CACHE_AMOUNT_PER_ADDRESS.insert({address, 0});
    }
}

void fillLydraLockedCacheForWallet()
{
    auto wallets = GetWallets();
    for (const auto& wallet : wallets)
    {
        auto currWallet = interfaces::MakeWallet(wallet);
        std::vector<std::string> spendableAddresses{};
        std::vector<std::string> allAddresses{};
        bool tmp;
        currWallet->tryGetAvailableAddresses(spendableAddresses, allAddresses, tmp);
        Lydra l;
        for (const auto& addr : allAddresses)
        {
            uint64_t amount;
            l.getLockedHydraAmountPerAddress(addr, amount);
            LYDRA_LOCKED_CACHE_AMOUNT_PER_ADDRESS.insert({addr, amount});
        }
    }

    LYDRA_LOCKED_CACHE_FILLED = true;
}

uint64_t getAllLydraLockedCache() 
{
    if (!LYDRA_LOCKED_CACHE_FILLED) fillLydraLockedCacheForWallet();

    uint64_t sum = 0;
    for (const auto& pair : LYDRA_LOCKED_CACHE_AMOUNT_PER_ADDRESS) {
        sum += pair.second;
    }

    return sum;
}

Lydra::Lydra()
{
    this->m_contractAbi.loads(LYDRA_CONTRACT_ABI);
}

bool Lydra::getMintDatahex(std::string& datahex)
{
    datahex = this->getContractFunctionHex(MINT);
    return true;
}

bool Lydra::getBurnDatahex(std::string& datahex, int64_t amount)
{
    if (amount == -1) {
        datahex = this->getContractFunctionHex(BURN_ALL);
        return true;
    } else {
        std::vector<std::string> params{std::to_string(amount)};
        std::vector<std::vector<std::string>> values{params};
        this->generateCallString(values, datahex, BURN);
        return true;
    }
}

bool Lydra::getLockedHydraAmountPerAddress(std::string address, uint64_t& amount)
{
    std::vector<std::string> params{address};
    std::string callString{};
    std::vector<std::vector<std::string>> values{params};
    bool status = this->generateCallString(values, callString, LOCKED_BALANCE);

    if (status) {
        std::vector<ResultExecute> result = CallContract(uintToh160(Params().GetConsensus().lydraAddress), ParseHex(callString));

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