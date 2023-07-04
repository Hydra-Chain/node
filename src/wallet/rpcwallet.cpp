// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <httpserver.h>
#include <init.h>
#include <interfaces/chain.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <miner.h>
#include <net.h>
#include <node/transaction.h>
#include <outputtype.h>
#include <policy/feerate.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <qtum/qtumdelegation.h>
#include <rpc/contract_util.h>
#include <rpc/mining.h>
#include <rpc/rawtransaction.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <script/sign.h>
#include <shutdown.h>
#include <timedata.h>
#include <util/bip32.h>
#include <util/moneystr.h>
#include <util/signstr.h>
#include <util/system.h>
#include <util/tokenstr.h>
#include <util/vector.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/feebumper.h>
#include <wallet/psbtwallet.h>
#include <wallet/rpcwallet.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>

#include <stdint.h>

#include <boost/optional.hpp>

#include <univalue.h>

#include "locktrip/dgp.h"
#include "locktrip/lydra.h"
#include "locktrip/price-oracle.h"

#include <functional>

#include <iostream>

static const std::string WALLET_ENDPOINT_BASE = "/wallet/";

bool GetWalletNameFromJSONRPCRequest(const JSONRPCRequest& request, std::string& wallet_name)
{
    if (request.URI.substr(0, WALLET_ENDPOINT_BASE.size()) == WALLET_ENDPOINT_BASE) {
        // wallet endpoint was used
        wallet_name = urlDecode(request.URI.substr(WALLET_ENDPOINT_BASE.size()));
        return true;
    }
    return false;
}

std::shared_ptr<CWallet> GetWalletForJSONRPCRequest(const JSONRPCRequest& request)
{
    std::string wallet_name;
    if (GetWalletNameFromJSONRPCRequest(request, wallet_name)) {
        std::shared_ptr<CWallet> pwallet = GetWallet(wallet_name);
        if (!pwallet) throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Requested wallet does not exist or is not loaded");
        return pwallet;
    }

    std::vector<std::shared_ptr<CWallet>> wallets = GetWallets();
    return wallets.size() == 1 || (request.fHelp && wallets.size() > 0) ? wallets[0] : nullptr;
}

std::string HelpRequiringPassphrase(CWallet* const pwallet)
{
    return pwallet && pwallet->IsCrypted() ? "\nRequires wallet passphrase to be set with walletpassphrase call." : "";
}

bool EnsureWalletIsAvailable(CWallet* const pwallet, bool avoidException)
{
    if (pwallet) return true;
    if (avoidException) return false;
    if (!HasWallets()) {
        throw JSONRPCError(
            RPC_METHOD_NOT_FOUND, "Method not found (wallet method is disabled because no wallet is loaded)");
    }
    throw JSONRPCError(RPC_WALLET_NOT_SPECIFIED,
        "Wallet file not specified (must request wallet RPC through /wallet/<filename> uri-path).");
}

void EnsureWalletIsUnlocked(CWallet* const pwallet)
{
    if (pwallet->IsLocked()) {
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
    }
    if (pwallet->m_wallet_unlock_staking_only) {
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Wallet is unlocked for staking only.");
    }
}

static void WalletTxToJSON(interfaces::Chain& chain, interfaces::Chain::Lock& locked_chain, const CWalletTx& wtx, UniValue& entry)
{
    int confirms = wtx.GetDepthInMainChain(locked_chain);
    entry.pushKV("confirmations", confirms);
    if (wtx.IsCoinBase() || wtx.IsCoinStake())
        entry.pushKV("generated", true);
    if (confirms > 0) {
        entry.pushKV("blockhash", wtx.hashBlock.GetHex());
        entry.pushKV("blockindex", wtx.nIndex);
        int64_t block_time;
        bool found_block = chain.findBlock(wtx.hashBlock, nullptr /* block */, &block_time);
        assert(found_block);
        entry.pushKV("blocktime", block_time);
    } else {
        entry.pushKV("trusted", wtx.IsTrusted(locked_chain));
    }
    uint256 hash = wtx.GetHash();
    entry.pushKV("txid", hash.GetHex());
    UniValue conflicts(UniValue::VARR);
    for (const uint256& conflict : wtx.GetConflicts())
        conflicts.push_back(conflict.GetHex());
    entry.pushKV("walletconflicts", conflicts);
    entry.pushKV("time", wtx.GetTxTime());
    entry.pushKV("timereceived", (int64_t)wtx.nTimeReceived);

    // Add opt-in RBF status
    std::string rbfStatus = "no";
    if (confirms <= 0) {
        LOCK(mempool.cs);
        RBFTransactionState rbfState = IsRBFOptIn(*wtx.tx, mempool);
        if (rbfState == RBFTransactionState::UNKNOWN)
            rbfStatus = "unknown";
        else if (rbfState == RBFTransactionState::REPLACEABLE_BIP125)
            rbfStatus = "yes";
    }
    entry.pushKV("bip125-replaceable", rbfStatus);

    for (const std::pair<const std::string, std::string>& item : wtx.mapValue)
        entry.pushKV(item.first, item.second);
}

static std::string LabelFromValue(const UniValue& value)
{
    std::string label = value.get_str();
    if (label == "*")
        throw JSONRPCError(RPC_WALLET_INVALID_LABEL_NAME, "Invalid label name");
    return label;
}

bool SetDefaultPayForContractAddress(CWallet* const pwallet, interfaces::Chain::Lock& locked_chain, CCoinControl& coinControl)
{
    // Set default coin to pay for the contract
    // Select any valid unspent output that can be used to pay for the contract
    std::vector<COutput> vecOutputs;
    coinControl.fAllowOtherInputs = true;

    assert(pwallet != NULL);
    pwallet->AvailableCoins(locked_chain, vecOutputs, false, &coinControl, true);

    for (const COutput& out : vecOutputs) {
        CTxDestination destAdress;
        const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
        bool fValidAddress = out.fSpendable && ExtractDestination(scriptPubKey, destAdress) && IsValidContractSenderAddress(destAdress);

        if (!fValidAddress)
            continue;

        coinControl.Select(COutPoint(out.tx->GetHash(), out.i));
        break;
    }

    return coinControl.HasSelected();
}

bool SetDefaultSignSenderAddress(CWallet* const pwallet, interfaces::Chain::Lock& locked_chain, CTxDestination& destAdress, CCoinControl& coinControl)
{
    // Set default sender address if none provided
    // Select any valid unspent output that can be used for contract sender address
    std::vector<COutput> vecOutputs;
    coinControl.fAllowOtherInputs = true;

    assert(pwallet != NULL);
    pwallet->AvailableCoins(locked_chain, vecOutputs, false, &coinControl, true);

    for (const COutput& out : vecOutputs) {
        const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
        bool fValidAddress = out.fSpendable && ExtractDestination(scriptPubKey, destAdress) && IsValidContractSenderAddress(destAdress);

        if (!fValidAddress)
            continue;
        break;
    }

    return !boost::get<CNoDestination>(&destAdress);
}

static UniValue getnewaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            RPCHelpMan{
                "getnewaddress",
                "\nReturns a new HYDRA address for receiving payments.\n"
                "If 'label' is specified, it is added to the address book \n"
                "so payments received with the address will be associated with 'label'.\n",
                {
                    {"label", RPCArg::Type::STR, /* default */ "\"\"", "The label name for the address to be linked to. It can also be set to the empty string \"\" to represent the default label. The label does not need to exist, it will be created if there is no label by the given name."},
                    {"address_type", RPCArg::Type::STR, /* default */ "set by -addresstype", "The address type to use. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                },
                RPCResult{
                    RPCResult::Type::STR, "address", "The new HYDRA address"},
                RPCExamples{
                    HelpExampleCli("getnewaddress", "") + HelpExampleRpc("getnewaddress", "")},
            }
                .ToString());

    LOCK(pwallet->cs_wallet);

    if (!pwallet->CanGetAddresses()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: This wallet has no available keys");
    }

    // Parse the label first so we don't generate a key if there's an error
    std::string label;
    if (!request.params[0].isNull())
        label = LabelFromValue(request.params[0]);

    OutputType output_type = pwallet->m_default_address_type;
    if (!request.params[1].isNull()) {
        if (!ParseOutputType(request.params[1].get_str(), output_type)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", request.params[1].get_str()));
        }
    }

    if (!pwallet->IsLocked()) {
        pwallet->TopUpKeyPool();
    }

    // Generate a new key that is added to wallet
    CPubKey newKey;
    if (!pwallet->GetKeyFromPool(newKey)) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    }
    pwallet->LearnRelatedScripts(newKey, output_type);
    CTxDestination dest = GetDestinationForKey(newKey, output_type);

    pwallet->SetAddressBook(dest, label, "receive");

    return EncodeDestination(dest);
}

static UniValue getrawchangeaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            RPCHelpMan{
                "getrawchangeaddress",
                "\nReturns a new HYDRA address, for receiving change.\n"
                "This is for use with raw transactions, NOT normal use.\n",
                {
                    {"address_type", RPCArg::Type::STR, /* default */ "set by -changetype", "The address type to use. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                },
                RPCResult{
                    RPCResult::Type::STR, "address", "The address"},
                RPCExamples{
                    HelpExampleCli("getrawchangeaddress", "") + HelpExampleRpc("getrawchangeaddress", "")},
            }
                .ToString());

    LOCK(pwallet->cs_wallet);

    if (!pwallet->CanGetAddresses(true)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: This wallet has no available keys");
    }

    if (!pwallet->IsLocked()) {
        pwallet->TopUpKeyPool();
    }

    OutputType output_type = pwallet->m_default_change_type != OutputType::CHANGE_AUTO ? pwallet->m_default_change_type : pwallet->m_default_address_type;
    if (!request.params[0].isNull()) {
        if (!ParseOutputType(request.params[0].get_str(), output_type)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", request.params[0].get_str()));
        }
    }

    CReserveKey reservekey(pwallet);
    CPubKey vchPubKey;
    if (!reservekey.GetReservedKey(vchPubKey, true))
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");

    reservekey.KeepKey();

    pwallet->LearnRelatedScripts(vchPubKey, output_type);
    CTxDestination dest = GetDestinationForKey(vchPubKey, output_type);

    return EncodeDestination(dest);
}


static UniValue setlabel(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            RPCHelpMan{
                "setlabel",
                "\nSets the label associated with the given address.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The HYDRA address to be associated with a label."},
                    {"label", RPCArg::Type::STR, RPCArg::Optional::NO, "The label to assign to the address."},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("setlabel", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\" \"tabby\"") + HelpExampleRpc("setlabel", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\", \"tabby\"")},
            }
                .ToString());

    LOCK(pwallet->cs_wallet);

    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid HYDRA address");
    }

    std::string label = LabelFromValue(request.params[1]);

    if (IsMine(*pwallet, dest)) {
        pwallet->SetAddressBook(dest, label, "receive");
    } else {
        pwallet->SetAddressBook(dest, label, "send");
    }

    return NullUniValue;
}


static CTransactionRef SendMoney(interfaces::Chain::Lock& locked_chain, CWallet* const pwallet, const CTxDestination& address, CAmount nValue, bool fSubtractFeeFromAmount, const CCoinControl& coin_control, mapValue_t mapValue, std::string fromAccount, bool hasSender)
{
    CAmount curBalance = pwallet->GetBalance();

    // Check amount
    if (nValue <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    if (nValue > curBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    if (pwallet->GetBroadcastTransactions() && !g_connman) {
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");
    }

    if (pwallet->m_wallet_unlock_staking_only) {
        std::string strError = _("Error: Wallet unlocked for staking only, unable to create transaction.");
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

    // Parse Bitcoin address
    CScript scriptPubKey = GetScriptForDestination(address);

    // Create and send the transaction
    CReserveKey reservekey(pwallet);
    CAmount nFeeRequired;
    std::string strError;
    std::vector<CRecipient> vecSend;
    int nChangePosRet = -1;
    CRecipient recipient = {scriptPubKey, nValue, fSubtractFeeFromAmount};
    vecSend.push_back(recipient);
    CTransactionRef tx;
    if (!pwallet->CreateTransaction(locked_chain, vecSend, tx, reservekey, nFeeRequired, nChangePosRet, strError, coin_control, true, 0, hasSender)) {
        if (!fSubtractFeeFromAmount && nValue + nFeeRequired > curBalance)
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s", FormatMoney(nFeeRequired));
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    CValidationState state;
    if (!pwallet->CommitTransaction(tx, std::move(mapValue), {} /* orderForm */, reservekey, g_connman.get(), state)) {
        strError = strprintf("Error: The transaction was rejected! Reason given: %s", FormatStateMessage(state));
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    return tx;
}

void SplitRemainder(std::vector<CRecipient>& vecSend, CAmount& remainder, CAmount maxValue)
{
    if (remainder > 0) {
        for (int i = vecSend.size() - 1; i >= 0; i--) {
            CAmount diffAmount = maxValue - vecSend[i].nAmount;
            if (diffAmount > 0) {
                if ((remainder - diffAmount) > 0) {
                    vecSend[i].nAmount = vecSend[i].nAmount + diffAmount;
                    remainder -= diffAmount;
                } else {
                    vecSend[i].nAmount = vecSend[i].nAmount + remainder;
                    remainder = 0;
                }
            }

            if (remainder <= 0)
                break;
        }
    }
}

static CTransactionRef SplitUTXOs(interfaces::Chain::Lock& locked_chain, CWallet* const pwallet, const CTxDestination& address, CAmount nValue, CAmount maxValue, const CCoinControl& coin_control, CAmount nTotal, int maxOutputs, CAmount& nSplited)
{
    // Check amount
    if (nValue <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    if (nValue > nTotal)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    // Parse Qtum address
    CScript scriptPubKey = GetScriptForDestination(address);

    // Split into utxos with nValue
    CAmount nFeeRequired = 0;
    std::string strError;
    std::vector<CRecipient> vecSend;
    int nChangePosRet = -1;
    int numOfRecipients = static_cast<int>(nTotal / nValue);

    // Compute the number of recipients
    CAmount remainder = nTotal % nValue;
    if (remainder == 0 && numOfRecipients > 0) {
        numOfRecipients -= 1;
        remainder = nValue;
    }
    if (numOfRecipients > maxOutputs) {
        numOfRecipients = maxOutputs;
        remainder = 0;
    }

    // Split coins between recipients
    CAmount nTxAmount = 0;
    nSplited = 0;
    CRecipient recipient = {scriptPubKey, nValue, false};
    for (int i = 0; i < numOfRecipients; i++) {
        vecSend.push_back(recipient);
    }
    SplitRemainder(vecSend, remainder, maxValue);

    // Get the total amount of the outputs
    for (CRecipient rec : vecSend) {
        nTxAmount += rec.nAmount;
    }

    // Create the transaction
    CTransactionRef tx;
    CReserveKey reservekey(pwallet);
    if ((nTxAmount + ::maxTxFee) <= nTotal) {
        if (!pwallet->CreateTransaction(locked_chain, vecSend, tx, reservekey, nFeeRequired, nChangePosRet, strError, coin_control, true, 0, true)) {
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
        }
        nSplited = nFeeRequired;
    } else if (vecSend.size() > 0) {
        // Pay the fee for the tx with the last recipient
        CRecipient lastRecipient = vecSend[vecSend.size() - 1];
        lastRecipient.fSubtractFeeFromAmount = true;
        vecSend[vecSend.size() - 1] = lastRecipient;
        if (!pwallet->CreateTransaction(locked_chain, vecSend, tx, reservekey, nFeeRequired, nChangePosRet, strError, coin_control, true, 0, true)) {
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
        }

        // Combine the last 2 outputs when the last output have value less than nValue due to paying the fee
        if (vecSend.size() >= 2) {
            if ((lastRecipient.nAmount - nFeeRequired) < nValue) {
                bool payFeeRemainder = (nTotal - nTxAmount) > nFeeRequired * 1.1;
                if (payFeeRemainder) {
                    // Pay the fee with the remainder
                    lastRecipient.fSubtractFeeFromAmount = false;
                    vecSend.pop_back();
                    vecSend.push_back(lastRecipient);
                } else {
                    // Combine the last 2 outputs
                    CAmount nValueLast2 = lastRecipient.nAmount + vecSend[vecSend.size() - 2].nAmount;
                    lastRecipient.nAmount = lastRecipient.nAmount + nFeeRequired;
                    lastRecipient.fSubtractFeeFromAmount = true;
                    nValueLast2 -= lastRecipient.nAmount;
                    vecSend.pop_back();
                    vecSend.pop_back();
                    vecSend.push_back(lastRecipient);

                    // Split the rest with the others
                    SplitRemainder(vecSend, nValueLast2, maxValue);
                }

                if ((!pwallet->CreateTransaction(locked_chain, vecSend, tx, reservekey, nFeeRequired, nChangePosRet, strError, coin_control, true, 0, true))) {
                    throw JSONRPCError(RPC_WALLET_ERROR, strError);
                }
                if (payFeeRemainder) {
                    nSplited = nFeeRequired;
                }
            }
        }
    }

    // Compute the splited amount
    for (CRecipient rec : vecSend) {
        nSplited += rec.nAmount;
    }

    // Send the transaction
    CValidationState state;
    if (!pwallet->CommitTransaction(tx, {} /* mapValue */, {} /* orderForm */, reservekey, g_connman.get(), state)) {
        strError = strprintf("Error: The transaction was rejected! Reason given: %s", FormatStateMessage(state));
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

    return tx;
}

static UniValue sendtoaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 10)
        throw std::runtime_error(
            RPCHelpMan{
                "sendtoaddress",
                "\nSend an amount to a given address." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The HYDRA address to send to."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in " + CURRENCY_UNIT + " to send. eg 0.1"},
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment used to store what the transaction is for.\n"
                                                                                        "                             This is not part of the transaction, just kept in your wallet."},
                    {"comment_to", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment to store the name of the person or organization\n"
                                                                                           "                             to which you're sending the transaction. This is not part of the \n"
                                                                                           "                             transaction, just kept in your wallet."},
                    {"subtractfeefromamount", RPCArg::Type::BOOL, /* default */ "false", "The fee will be deducted from the amount being sent.\n"
                                                                                         "                             The recipient will receive less HYDRA than you enter in the amount field."},
                    {"replaceable", RPCArg::Type::BOOL, /* default */ "fallback to wallet's default", "Allow this transaction to be replaced by a transaction with higher fees via BIP 125"},
                    {"conf_target", RPCArg::Type::NUM, /* default */ "fallback to wallet's default", "Confirmation target (in blocks)"},
                    {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
                                                                                "       \"UNSET\"\n"
                                                                                "       \"ECONOMICAL\"\n"
                                                                                "       \"CONSERVATIVE\""},
                    {"senderaddress", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "The HYDRA address that will be used to send money from."},
                    {"changeToSender", RPCArg::Type::BOOL, /* default */ "false", "Return the change to the sender."},
                },
                RPCResult{
                    RPCResult::Type::STR_HEX, "txid", "The transaction id."},
                RPCExamples{
                    HelpExampleCli("sendtoaddress", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\" 0.1") + HelpExampleCli("sendtoaddress", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\" 0.1 \"donation\" \"seans outpost\"") + HelpExampleCli("sendtoaddress", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\" 0.1 \"\" \"\" true") + HelpExampleCli("sendtoaddress", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\", 0.1, \"donation\", \"seans outpost\", false, null, null, \"\", \"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\", true") + HelpExampleRpc("sendtoaddress", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\", 0.1, \"donation\", \"seans outpost\"" + HelpExampleRpc("sendtoaddress", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\", 0.1, \"donation\", \"seans outpost\", false, null, null, \"\", \"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\", true"))},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid HYDRA address");
    }

    // Amount
    CAmount nAmount = AmountFromValue(request.params[1]);
    if (nAmount <= 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");

    // Wallet comments
    mapValue_t mapValue;
    if (!request.params[2].isNull() && !request.params[2].get_str().empty())
        mapValue["comment"] = request.params[2].get_str();
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        mapValue["to"] = request.params[3].get_str();

    bool fSubtractFeeFromAmount = false;
    if (!request.params[4].isNull()) {
        fSubtractFeeFromAmount = request.params[4].get_bool();
    }

    CCoinControl coin_control;
    if (!request.params[5].isNull()) {
        coin_control.m_signal_bip125_rbf = request.params[5].get_bool();
    }

    if (!request.params[6].isNull()) {
        coin_control.m_confirm_target = ParseConfirmTarget(request.params[6]);
    }

    if (request.params.size() > 7 && !request.params[7].isNull()) {
        std::string estimate_mode = request.params[7].get_str();
        if (!estimate_mode.empty() && !FeeModeFromString(estimate_mode, coin_control.m_fee_mode)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
    }

    bool fHasSender = false;
    CTxDestination senderAddress;
    if (request.params.size() > 8 && !request.params[8].isNull()) {
        senderAddress = DecodeDestination(request.params[8].get_str());
        if (!IsValidDestination(senderAddress))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid HYDRA address to send from");
        else
            fHasSender = true;
    }

    bool fChangeToSender = false;
    if (request.params.size() > 9 && !request.params[9].isNull()) {
        fChangeToSender = request.params[9].get_bool();
    }

    if (fHasSender) {
        // find a UTXO with sender address

        UniValue results(UniValue::VARR);
        std::vector<COutput> vecOutputs;

        coin_control.fAllowOtherInputs = true;

        assert(pwallet != NULL);
        pwallet->AvailableCoins(*locked_chain, vecOutputs, false, NULL, true);

        for (const COutput& out : vecOutputs) {
            CTxDestination destAdress;
            const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
            bool fValidAddress = ExtractDestination(scriptPubKey, destAdress);

            if (!fValidAddress || senderAddress != destAdress)
                continue;

            coin_control.Select(COutPoint(out.tx->GetHash(), out.i));

            break;
        }

        if (!coin_control.HasSelected()) {
            throw JSONRPCError(RPC_TYPE_ERROR, "Sender address does not have any unspent outputs");
        }
        if (fChangeToSender) {
            coin_control.destChange = senderAddress;
        }
    }

    EnsureWalletIsUnlocked(pwallet);

    CTransactionRef tx = SendMoney(*locked_chain, pwallet, dest, nAmount, fSubtractFeeFromAmount, coin_control, std::move(mapValue), {} /* fromAccount */, fHasSender);
    return tx->GetHash().GetHex();
}

static UniValue splitutxosforaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 4)
        throw std::runtime_error(
            RPCHelpMan{"splitutxosforaddress",
                "\nSplit an address coins into utxo between min and max value." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The hydra address to split utxos."},
                    {"minValue", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "Select utxo which value is smaller than value (minimum 0.1 COIN)"},
                    {"maxValue", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "Select utxo which value is greater than value (minimum 0.1 COIN)"},
                    {"maxOutputs", RPCArg::Type::NUM, RPCArg::Optional::NO, "Maximum outputs to create"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The hex-encoded transaction id"},
                        {RPCResult::Type::STR_AMOUNT, "selected", "Selected amount of coins"},
                        {RPCResult::Type::STR_AMOUNT, "splited", "Splited amount of coins"},
                    }},
                RPCExamples{
                    HelpExampleCli("splitutxosforaddress", "\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 100 200") + HelpExampleCli("splitutxosforaddress", "\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 100 200 100") + HelpExampleRpc("splitutxosforaddress", "\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 100 200") + HelpExampleRpc("splitutxosforaddress", "\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 100 200 100")}}
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    // Address
    CTxDestination address = DecodeDestination(request.params[0].get_str());

    if (!IsValidDestination(address)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid HYDRA address");
    }
    CScript scriptPubKey = GetScriptForDestination(address);
    if (!IsMine(*pwallet, scriptPubKey)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Address not found in wallet");
    }

    // minimum value
    CAmount minValue = AmountFromValue(request.params[1]);

    // maximum value
    CAmount maxValue = AmountFromValue(request.params[2]);

    if (minValue < COIN / 10 || maxValue <= 0 || minValue > maxValue) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid values for minimum and maximum");
    }

    // Maximum outputs
    int maxOutputs = request.params.size() > 3 ? request.params[3].get_int() : 100;
    if (maxOutputs < 1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid value for maximum outputs");
    }

    // Amount
    CAmount nSplitAmount = minValue;
    CAmount nRequiredAmount = nSplitAmount * maxOutputs;

    CCoinControl coin_control;
    coin_control.destChange = address;

    // Find UTXOs for a address with value smaller than minValue and greater then maxValue
    std::vector<COutput> vecOutputs;
    coin_control.fAllowOtherInputs = true;

    assert(pwallet != NULL);
    pwallet->AvailableCoins(*locked_chain, vecOutputs, false, NULL, true);

    CAmount total = 0;
    CAmount nSelectedAmount = 0;
    for (const COutput& out : vecOutputs) {
        CTxDestination destAdress;
        const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
        bool fValidAddress = ExtractDestination(scriptPubKey, destAdress);

        CAmount val = out.tx->tx.get()->vout[out.i].nValue;
        if (!fValidAddress || address != destAdress || (val >= minValue && val <= maxValue))
            continue;

        if (nSelectedAmount <= nRequiredAmount) {
            coin_control.Select(COutPoint(out.tx->GetHash(), out.i));
            nSelectedAmount += val;
        }
        total += val;
    }

    CAmount splited = 0;
    UniValue obj(UniValue::VOBJ);
    if (coin_control.HasSelected() && nSplitAmount < nSelectedAmount) {
        EnsureWalletIsUnlocked(pwallet);
        CTransactionRef tx = SplitUTXOs(*locked_chain, pwallet, address, nSplitAmount, maxValue, coin_control, nSelectedAmount, maxOutputs, splited);
        obj.pushKV("txid", tx->GetHash().GetHex());
    }

    obj.pushKV("selected", FormatMoney(total));
    obj.pushKV("splited", FormatMoney(splited));
    return obj;
}

static UniValue createcontract(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    QtumDGP qtumDGP(globalState.get(), fGettingValuesDGP);
    uint64_t blockGasLimit = qtumDGP.getBlockGasLimit(chainActive.Height());
    uint64_t minGasPrice = CAmount(qtumDGP.getMinGasPrice(chainActive.Height()));
    PriceOracle oracle;
    uint64_t oracleGasPrice;
    oracle.getPrice(oracleGasPrice);
    CAmount defaultGasPrice = (minGasPrice > DEFAULT_GAS_PRICE) ? minGasPrice : oracleGasPrice;
    Dgp dgp;
    CAmount gasPriceBuffer;
    dgp.calculateGasPriceBuffer(defaultGasPrice, gasPriceBuffer);
    CAmount nGasPrice = gasPriceBuffer + defaultGasPrice;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 6)
        throw std::runtime_error(
            RPCHelpMan{
                "createcontract",
                "\nCreate a contract with bytcode." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"bytecode", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "contract bytcode."},
                    {"gasLimit", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "gasLimit, default: " + i64tostr(DEFAULT_GAS_LIMIT_OP_CREATE) + ", max: " + i64tostr(blockGasLimit)},
                    {"senderaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "The HYDRA address that will be used to create the contract."},
                    {"broadcast", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Whether to broadcast the transaction or not."},
                    {"changeToSender", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Return the change to the sender."},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "txid", "The transaction id. Only returned when wallet private keys are enabled."},
                                {RPCResult::Type::STR, "sender", CURRENCY_UNIT + " address of the sender"},
                                {RPCResult::Type::STR_HEX, "hash160", "Ripemd-160 hash of the sender"},
                                {RPCResult::Type::STR, "address", "Expected contract address"},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("createcontract", "\"60606040525b33600060006101000a81548173ffffffffffffffffffffffffffffffffffffffff02191690836c010000000000000000000000009081020402179055506103786001600050819055505b600c80605b6000396000f360606040526008565b600256\"") + HelpExampleCli("createcontract", "\"60606040525b33600060006101000a81548173ffffffffffffffffffffffffffffffffffffffff02191690836c010000000000000000000000009081020402179055506103786001600050819055505b600c80605b6000396000f360606040526008565b600256\" 6000000 \"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\" true")},
            }
                .ToString());


    std::string bytecode = request.params[0].get_str();

    if (bytecode.size() % 2 != 0 || !CheckHex(bytecode))
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid data (data not hex)");

    uint64_t nGasLimit = DEFAULT_GAS_LIMIT_OP_CREATE;
    if (request.params.size() > 1) {
        nGasLimit = request.params[1].get_int64();
        if (nGasLimit > blockGasLimit)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid value for gasLimit (Maximum is: " + i64tostr(blockGasLimit) + ")");
        if (nGasLimit < MINIMUM_GAS_LIMIT)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid value for gasLimit (Minimum is: " + i64tostr(MINIMUM_GAS_LIMIT) + ")");
        if (nGasLimit <= 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid value for gasLimit");
    }

    bool fHasSender = false;
    CTxDestination senderAddress;
    if (request.params.size() > 2) {
        senderAddress = DecodeDestination(request.params[2].get_str());
        if (!IsValidDestination(senderAddress))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid HYDRA address to send from");
        if (!IsValidContractSenderAddress(senderAddress))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid contract sender address. Only P2PK and P2PKH allowed");
        else
            fHasSender = true;
    }

    bool fBroadcast = true;
    if (request.params.size() > 3) {
        fBroadcast = request.params[3].get_bool();
    }

    bool fChangeToSender = true;
    if (request.params.size() > 4) {
        fChangeToSender = request.params[4].get_bool();
    }

    CCoinControl coinControl;
    CTxDestination signSenderAddress = CNoDestination();

    if (fHasSender) {
        // find a UTXO with sender address
        std::vector<COutput> vecOutputs;

        coinControl.fAllowOtherInputs = true;

        assert(pwallet != NULL);
        pwallet->AvailableCoins(*locked_chain, vecOutputs, false, NULL, true);

        for (const COutput& out : vecOutputs) {
            CTxDestination destAdress;
            const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
            bool fValidAddress = out.fSpendable && ExtractDestination(scriptPubKey, destAdress);

            if (!fValidAddress || senderAddress != destAdress)
                continue;

            coinControl.Select(COutPoint(out.tx->GetHash(), out.i));

            break;
        }

        if (coinControl.HasSelected()) {
            // Change to the sender
            if (fChangeToSender) {
                coinControl.destChange = senderAddress;
            }
        } else {
            // Create op sender transaction when op sender is activated
            if (!(chainActive.Height() >= Params().GetConsensus().QIP5Height))
                throw JSONRPCError(RPC_TYPE_ERROR, "Sender address does not have any unspent outputs");
        }

        if (chainActive.Height() >= Params().GetConsensus().QIP5Height) {
            // Set the sender address
            signSenderAddress = senderAddress;
        }
    } else {
        if (chainActive.Height() >= Params().GetConsensus().QIP5Height) {
            // If no sender provided set to the default
            SetDefaultSignSenderAddress(pwallet, *locked_chain, signSenderAddress, coinControl);
        }
    }
    EnsureWalletIsUnlocked(pwallet);

    CAmount nGasFee = nGasPrice * nGasLimit;

    CAmount curBalance = pwallet->GetBalance();

    // Check amount
    if (nGasFee <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    if (nGasFee > curBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    // Select default coin that will pay for the contract if none selected
    if (!coinControl.HasSelected() && !SetDefaultPayForContractAddress(pwallet, *locked_chain, coinControl))
        throw JSONRPCError(RPC_TYPE_ERROR, "Does not have any P2PK or P2PKH unspent outputs to pay for the contract.");

    // Build OP_EXEC script
    CScript scriptPubKey = CScript() << CScriptNum(VersionVM::GetEVMDefault().toRaw()) << CScriptNum(nGasLimit) << ParseHex(bytecode) << OP_CREATE;

    if (chainActive.Height() >= Params().GetConsensus().QIP5Height) {
        if (IsValidDestination(signSenderAddress)) {
            CKeyID key_id = GetKeyForDestination(*pwallet, signSenderAddress);
            CKey key;
            if (!pwallet->GetKey(key_id, key)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");
            }
            std::vector<unsigned char> scriptSig;
            scriptPubKey = (CScript() << CScriptNum(addresstype::PUBKEYHASH) << ToByteVector(key_id) << ToByteVector(scriptSig) << OP_SENDER) + scriptPubKey;
        } else {
            // OP_SENDER will always be used when QIP5Height is active
            throw JSONRPCError(RPC_TYPE_ERROR, "Sender address fail to set for OP_SENDER.");
        }
    }

    // Create and send the transaction
    CReserveKey reservekey(pwallet);
    CAmount nFeeRequired;
    std::string strError;
    std::vector<CRecipient> vecSend;
    int nChangePosRet = -1;
    CRecipient recipient = {scriptPubKey, 0, false};
    vecSend.push_back(recipient);

    CTransactionRef tx;
    if (!pwallet->CreateTransaction(*locked_chain, vecSend, tx, reservekey, nFeeRequired, nChangePosRet, strError, coinControl, true, nGasFee, true, signSenderAddress)) {
        if (nFeeRequired > pwallet->GetBalance())
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds!", FormatMoney(nFeeRequired));
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

    CTxDestination txSenderDest;
    pwallet->GetSenderDest(tx, txSenderDest);

    if (fHasSender && !(senderAddress == txSenderDest)) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Sender could not be set, transaction was not committed!");
    }

    UniValue result(UniValue::VOBJ);
    if (fBroadcast) {
        CValidationState state;
        if (!pwallet->CommitTransaction(tx, {}, {}, reservekey, g_connman.get(), state))
            throw JSONRPCError(RPC_WALLET_ERROR, "Error: The transaction was rejected! This might happen if some of the coins in your wallet were already spent, such as if you used a copy of the wallet and coins were spent in the copy but not marked as spent here.");

        std::string txId = tx->GetHash().GetHex();
        result.pushKV("txid", txId);

        CTxDestination txSenderAdress(txSenderDest);
        CKeyID keyid = GetKeyForDestination(*pwallet, txSenderAdress);

        result.pushKV("sender", EncodeDestination(txSenderAdress));
        result.pushKV("hash160", HexStr(valtype(keyid.begin(), keyid.end())));

        std::vector<unsigned char> SHA256TxVout(32);
        std::vector<unsigned char> contractAddress(20);
        std::vector<unsigned char> txIdAndVout(tx->GetHash().begin(), tx->GetHash().end());
        uint32_t voutNumber = 0;
        for (const CTxOut& txout : tx->vout) {
            if (txout.scriptPubKey.HasOpCreate()) {
                std::vector<unsigned char> voutNumberChrs;
                if (voutNumberChrs.size() < sizeof(voutNumber)) voutNumberChrs.resize(sizeof(voutNumber));
                std::memcpy(voutNumberChrs.data(), &voutNumber, sizeof(voutNumber));
                txIdAndVout.insert(txIdAndVout.end(), voutNumberChrs.begin(), voutNumberChrs.end());
                break;
            }
            voutNumber++;
        }
        CSHA256().Write(txIdAndVout.data(), txIdAndVout.size()).Finalize(SHA256TxVout.data());
        CRIPEMD160().Write(SHA256TxVout.data(), SHA256TxVout.size()).Finalize(contractAddress.data());
        result.pushKV("address", HexStr(contractAddress));
    } else {
        std::string strHex = EncodeHexTx(*tx, RPCSerializationFlags());
        result.pushKV("raw transaction", strHex);
    }
    return result;
}

UniValue SendToContract(interfaces::Chain::Lock& locked_chain, CWallet* const pwallet, const UniValue& params, CAmount lydra_mint_amount = -1)
{
    QtumDGP qtumDGP(globalState.get(), fGettingValuesDGP);
    uint64_t blockGasLimit = qtumDGP.getBlockGasLimit(chainActive.Height());
    uint64_t minGasPrice = CAmount(qtumDGP.getMinGasPrice(chainActive.Height()));
    PriceOracle oracle;
    uint64_t oracleGasPrice;
    oracle.getPrice(oracleGasPrice);
    CAmount defaultGasPrice = (minGasPrice > DEFAULT_GAS_PRICE) ? minGasPrice : oracleGasPrice;
    Dgp dgp;
    CAmount gasPriceBuffer;
    dgp.calculateGasPriceBuffer(defaultGasPrice, gasPriceBuffer);
    CAmount nGasPrice = gasPriceBuffer + defaultGasPrice;

    std::string contractaddress = params[0].get_str();

    if (contractaddress.size() != 40 || !CheckHex(contractaddress))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Incorrect contract address");

    dev::Address addrAccount(contractaddress);
    if (!globalState->addressInUse(addrAccount))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "contract address does not exist");

    std::string datahex = params[1].get_str();
    if (datahex.size() % 2 != 0 || !CheckHex(datahex))
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid data (data not hex)");

    CAmount nAmount = 0;
    if (params.size() > 2) {
        nAmount = AmountFromValue(params[2]);
        if (nAmount < 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");
    }

    if (lydra_mint_amount != -1) {
        nAmount = lydra_mint_amount;
    }

    uint64_t nGasLimit = DEFAULT_GAS_LIMIT_OP_SEND;
    if (params.size() > 3) {
        nGasLimit = params[3].get_int64();
        if (nGasLimit > blockGasLimit)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid value for gasLimit (Maximum is: " + i64tostr(blockGasLimit) + ")");
        if (nGasLimit < MINIMUM_GAS_LIMIT)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid value for gasLimit (Minimum is: " + i64tostr(MINIMUM_GAS_LIMIT) + ")");
        if (nGasLimit <= 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid value for gasLimit");
    }

    bool fHasSender = false;
    CTxDestination senderAddress;
    if (params.size() > 4) {
        senderAddress = DecodeDestination(params[4].get_str());
        if (!IsValidDestination(senderAddress))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid HYDRA address to send from");
        if (!IsValidContractSenderAddress(senderAddress))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid contract sender address. Only P2PK and P2PKH allowed");
        else
            fHasSender = true;
    }

    bool fBroadcast = true;
    if (params.size() > 5) {
        fBroadcast = params[5].get_bool();
    }

    bool fChangeToSender = true;
    if (params.size() > 6) {
        fChangeToSender = params[6].get_bool();
    }

    CCoinControl coinControl;
    CTxDestination signSenderAddress = CNoDestination();

    if (fHasSender) {
        // Find a UTXO with sender address
        std::vector<COutput> vecOutputs;

        coinControl.fAllowOtherInputs = true;

        assert(pwallet != NULL);
        pwallet->AvailableCoins(locked_chain, vecOutputs, false, NULL, true);

        for (const COutput& out : vecOutputs) {
            CTxDestination destAdress;
            const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
            bool fValidAddress = out.fSpendable && ExtractDestination(scriptPubKey, destAdress);

            if (!fValidAddress || senderAddress != destAdress)
                continue;

            coinControl.Select(COutPoint(out.tx->GetHash(), out.i));

            break;
        }

        if (coinControl.HasSelected()) {
            // Change to the sender
            if (fChangeToSender) {
                coinControl.destChange = senderAddress;
            }
        } else {
            // Create op sender transaction when op sender is activated
            if (!(chainActive.Height() >= Params().GetConsensus().QIP5Height))
                throw JSONRPCError(RPC_TYPE_ERROR, "Sender address does not have any unspent outputs");
        }

        if (chainActive.Height() >= Params().GetConsensus().QIP5Height) {
            // Set the sender address
            signSenderAddress = senderAddress;
        }
    } else {
        if (chainActive.Height() >= Params().GetConsensus().QIP5Height) {
            // If no sender address provided set to the default sender address
            SetDefaultSignSenderAddress(pwallet, locked_chain, signSenderAddress, coinControl);
        }
    }

    EnsureWalletIsUnlocked(pwallet);

    CAmount nGasFee = nGasPrice * nGasLimit;

    CAmount curBalance = pwallet->GetBalance();

    // Check amount
    if (nGasFee <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount for gas fee");

    if (nAmount + nGasFee > curBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    // Select default coin that will pay for the contract if none selected
    if (!coinControl.HasSelected() && !SetDefaultPayForContractAddress(pwallet, locked_chain, coinControl))
        throw JSONRPCError(RPC_TYPE_ERROR, "Does not have any P2PK or P2PKH unspent outputs to pay for the contract.");

    // Build OP_EXEC_ASSIGN script
    CScript scriptPubKey = CScript() << CScriptNum(VersionVM::GetEVMDefault().toRaw()) << CScriptNum(nGasLimit) << ParseHex(datahex) << ParseHex(contractaddress) << OP_CALL;

    if (chainActive.Height() >= Params().GetConsensus().QIP5Height) {
        if (IsValidDestination(signSenderAddress)) {
            CKeyID key_id = GetKeyForDestination(*pwallet, signSenderAddress);
            CKey key;
            if (!pwallet->GetKey(key_id, key)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");
            }
            std::vector<unsigned char> scriptSig;
            scriptPubKey = (CScript() << CScriptNum(addresstype::PUBKEYHASH) << ToByteVector(key_id) << ToByteVector(scriptSig) << OP_SENDER) + scriptPubKey;
        } else {
            // OP_SENDER will always be used when QIP5Height is active
            throw JSONRPCError(RPC_TYPE_ERROR, "Sender address fail to set for OP_SENDER.");
        }
    }

    // Create and send the transaction
    CAmount nFeeRequired;
    std::string strError;
    std::vector<CRecipient> vecSend;
    int nChangePosRet = -1;
    CRecipient recipient = {scriptPubKey, nAmount, false};
    vecSend.push_back(recipient);
    CReserveKey reservekey(pwallet);

    CTransactionRef tx;
    if (!pwallet->CreateTransaction(locked_chain, vecSend, tx, reservekey, nFeeRequired, nChangePosRet, strError, coinControl, true, nGasFee, true, signSenderAddress, senderAddress)) {
        if (nFeeRequired > pwallet->GetBalance())
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds!", FormatMoney(nFeeRequired));
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

    CTxDestination txSenderDest;
    pwallet->GetSenderDest(tx, txSenderDest);

    if (fHasSender && !(senderAddress == txSenderDest)) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Sender could not be set, transaction was not committed!");
    }

    UniValue result(UniValue::VOBJ);

    if (fBroadcast) {
        CValidationState state;
        if (!pwallet->CommitTransaction(tx, {}, {}, reservekey, g_connman.get(), state))
        {
            strError = strprintf("Error: The transaction was rejected! %s", FormatStateMessage(state));
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
        }

        std::string txId = tx->GetHash().GetHex();
        result.pushKV("txid", txId);

        CTxDestination txSenderAdress(txSenderDest);
        CKeyID keyid = GetKeyForDestination(*pwallet, txSenderAdress);

        result.pushKV("sender", EncodeDestination(txSenderAdress));
        result.pushKV("hash160", HexStr(valtype(keyid.begin(), keyid.end())));
    } else {
        std::string strHex = EncodeHexTx(*tx, RPCSerializationFlags());
        result.pushKV("raw transaction", strHex);
    }

    return result;
}

/**
 * @brief The SendToken class Write token data
 */
class SendToken : public CallToken
{
public:
    SendToken(interfaces::Chain::Lock& _locked_chain,
        CWallet* const _pwallet) : locked_chain(_locked_chain),
                                   pwallet(_pwallet)
    {
    }

    bool execValid(const int& func, const bool& sendTo)
    {
        return sendTo ? func != -1 : CallToken::execValid(func, sendTo);
    }

    bool exec(const bool& sendTo, const std::map<std::string, std::string>& lstParams, std::string& result, std::string& message)
    {
        if (!sendTo)
            return CallToken::exec(sendTo, lstParams, result, message);

        UniValue params(UniValue::VARR);

        // Set address
        auto it = lstParams.find(paramAddress());
        if (it != lstParams.end())
            params.push_back(it->second);
        else
            return false;

        // Set data
        it = lstParams.find(paramDatahex());
        if (it != lstParams.end())
            params.push_back(it->second);
        else
            return false;

        // Set amount
        it = lstParams.find(paramAmount());
        if (it != lstParams.end()) {
            if (params.size() == 2)
                params.push_back(it->second);
            else
                return false;
        }

        // Set gas limit
        it = lstParams.find(paramGasLimit());
        if (it != lstParams.end()) {
            if (params.size() == 3) {
                UniValue param(UniValue::VNUM);
                param.setInt(atoi64(it->second));
                params.push_back(param);
            } else
                return false;
        }

        // Set sender
        it = lstParams.find(paramSender());
        if (it != lstParams.end()) {
            if (params.size() == 4)
                params.push_back(it->second);
            else
                return false;
        }

        // Set broadcast
        it = lstParams.find(paramBroadcast());
        if (it != lstParams.end()) {
            if (params.size() == 5) {
                bool val = it->second == "true" ? true : false;
                UniValue param(UniValue::VBOOL);
                param.setBool(val);
                params.push_back(param);
            } else
                return false;
        }

        // Set change to sender
        it = lstParams.find(paramChangeToSender());
        if (it != lstParams.end()) {
            if (params.size() == 6) {
                bool val = it->second == "true" ? true : false;
                UniValue param(UniValue::VBOOL);
                param.setBool(val);
                params.push_back(param);
            } else
                return false;
        }

        // Get execution result
        UniValue response = SendToContract(locked_chain, pwallet, params);
        if (!response.isObject() || !response.exists("txid"))
            return false;
        result = response["txid"].get_str();

        return true;
    }

private:
    interfaces::Chain::Lock& locked_chain;
    CWallet* const pwallet;
};

static UniValue sendtocontract(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    QtumDGP qtumDGP(globalState.get(), fGettingValuesDGP);
    uint64_t blockGasLimit = qtumDGP.getBlockGasLimit(chainActive.Height());

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 8)
        throw std::runtime_error(
            RPCHelpMan{
                "sendtocontract",
                "\nSend funds and data to a contract." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"contractaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address that will receive the funds and data."},
                    {"datahex", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "data to send."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The amount in " + CURRENCY_UNIT + " to send. eg 0.1, default: 0"},
                    {"gasLimit", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "gasLimit, default: " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + ", max: " + i64tostr(blockGasLimit)},
                    {"senderaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "The HYDRA address that will be used as sender."},
                    {"broadcast", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Whether to broadcast the transaction or not."},
                    {"changeToSender", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Return the change to the sender."},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "txid", "The transaction id. Only returned when wallet private keys are enabled."},
                                {RPCResult::Type::STR, "sender", CURRENCY_UNIT + " address of the sender"},
                                {RPCResult::Type::STR_HEX, "hash160", "Ripemd-160 hash of the sender"},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("sendtocontract", "\"c6ca2697719d00446d4ea51f6fac8fd1e9310214\" \"54f6127f\"") + HelpExampleCli("sendtocontract", "\"c6ca2697719d00446d4ea51f6fac8fd1e9310214\" \"54f6127f\" 12.0015 6000000 \"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\"")},
            }
                .ToString());

    return SendToContract(*locked_chain, pwallet, request.params);
}

static UniValue removedelegationforaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    QtumDGP qtumDGP(globalState.get(), fGettingValuesDGP);
    uint64_t blockGasLimit = qtumDGP.getBlockGasLimit(chainActive.Height());
    uint64_t minGasPrice = CAmount(qtumDGP.getMinGasPrice(chainActive.Height()));
    PriceOracle oracle;
    uint64_t oracleGasPrice;
    oracle.getPrice(oracleGasPrice);
    CAmount defaultGasPrice = (minGasPrice > DEFAULT_GAS_PRICE) ? minGasPrice : oracleGasPrice;
    Dgp dgp;
    CAmount gasPriceBuffer;
    dgp.calculateGasPriceBuffer(defaultGasPrice, gasPriceBuffer);
    CAmount nGasPrice = gasPriceBuffer + defaultGasPrice;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
            RPCHelpMan{
                "removedelegationforaddress",
                "\nRemove delegation for address." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The HYDRA address to remove delegation, the address will be used as sender too."},
                    {"gasLimit", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "gasLimit, default: " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + ", max: " + i64tostr(blockGasLimit)},
                    {"unlockAmount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "unlockAmount (in HYDRA), default: 0, pass -1 for whole amount"},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "txid", "The transaction id. Only returned when wallet private keys are enabled."},
                                {RPCResult::Type::STR, "sender", CURRENCY_UNIT + " address of the sender"},
                                {RPCResult::Type::STR_HEX, "hash160", "Ripemd-160 hash of the sender"},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("removedelegationforaddress", " \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 6000000") + 
                    HelpExampleCli("removedelegationforaddress", " \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 6000000 100")},
            }
                .ToString());

    // Get send to contract parameters for removing delegation for address
    UniValue params(UniValue::VARR);
    UniValue contractaddress = HexStr(Params().GetConsensus().GetDelegationsAddress(chainActive.Height()));
    UniValue datahex = QtumDelegation::BytecodeRemove();
    UniValue amount = 0;
    UniValue gasLimit = request.params.size() > 1 ? request.params[1] : DEFAULT_GAS_LIMIT_OP_SEND;
    UniValue senderaddress = request.params[0];
    CAmount unlockAmount;
    if (request.params.size() > 2 && (request.params[2].getValStr() == std::to_string(COIN) || request.params[2].getValStr() == "-1")) unlockAmount = -1;
    else unlockAmount = request.params.size() > 2 ? AmountFromValue(request.params[2]) : 0;

    // Parse the staker address
    CTxDestination destStaker = DecodeDestination(request.params[0].get_str());
    const CKeyID* pkhStaker = boost::get<CKeyID>(&destStaker);
    if (!pkhStaker) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid contract address for staker. Only P2PK and P2PKH allowed");
    }

    // Add the send to contract parameters to the list
    params.push_back(contractaddress);
    params.push_back(datahex);
    params.push_back(amount);
    params.push_back(gasLimit);
    params.push_back(senderaddress);

    if (unlockAmount != 0 && chainActive.Height() >= Params().GetConsensus().nLydraHeight) {
        Lydra lydraContract;
        std::string burnDatahex;
        lydraContract.getBurnDatahex(burnDatahex, unlockAmount);

        UniValue lydraParams(UniValue::VARR);
        lydraParams.push_back(HexStr(Params().GetConsensus().lydraAddress));
        lydraParams.push_back(burnDatahex);
        lydraParams.push_back(0);
        lydraParams.push_back(gasLimit);
        lydraParams.push_back(senderaddress);
        auto burn_res = SendToContract(*locked_chain, pwallet, lydraParams, -1);
        if (burn_res.isObject()) {
            if (unlockAmount == -1)
                clearLydraLockedCache(pkhStaker->GetReverseHex());
            else
                updateLydraLockedCache(unlockAmount, pkhStaker->GetReverseHex(), false);
        }
    }

    // Send to contract
    return SendToContract(*locked_chain, pwallet, params);
}

static UniValue setdelegateforaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    QtumDGP qtumDGP(globalState.get(), fGettingValuesDGP);
    uint64_t blockGasLimit = qtumDGP.getBlockGasLimit(chainActive.Height());
    uint64_t minGasPrice = CAmount(qtumDGP.getMinGasPrice(chainActive.Height()));
    PriceOracle oracle;
    uint64_t oracleGasPrice;
    oracle.getPrice(oracleGasPrice);
    CAmount defaultGasPrice = (minGasPrice > DEFAULT_GAS_PRICE) ? minGasPrice : oracleGasPrice;
    Dgp dgp;
    CAmount gasPriceBuffer;
    dgp.calculateGasPriceBuffer(defaultGasPrice, gasPriceBuffer);
    CAmount nGasPrice = gasPriceBuffer + defaultGasPrice;

    if (request.fHelp || request.params.size() < 3 || request.params.size() > 5)
        throw std::runtime_error(
            RPCHelpMan{
                "setdelegateforaddress",
                "\nSet delegate for address." +
                    HelpRequiringPassphrase(pwallet) + "\nWARNING: Minting will happen only if address balance is above 5M gas!\n",
                {
                    {"staker", RPCArg::Type::STR, RPCArg::Optional::NO, "The HYDRA address for the staker."},
                    {"fee", RPCArg::Type::NUM, RPCArg::Optional::NO, "Percentage of the reward that will be paid to the staker."},
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The HYDRA address that contain the coins that will be delegated to the staker, the address will be used as sender too."},
                    {"gasLimit", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "gasLimit, default: " + i64tostr(DEFAULT_GAS_LIMIT_OP_CREATE) + ", max: " + i64tostr(blockGasLimit)},
                    {"lockAmount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "lockAmount (in HYDRA), default: 0, pass -1 for whole amount"},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "txid", "The transaction id. Only returned when wallet private keys are enabled."},
                                {RPCResult::Type::STR, "sender", CURRENCY_UNIT + " address of the sender"},
                                {RPCResult::Type::STR_HEX, "hash160", "Ripemd-160 hash of the sender"},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("setdelegateforaddress", " \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 10 \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" 6000000") + 
                    HelpExampleCli("setdelegateforaddress", " \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 10 \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" 6000000 100")},
            }
                .ToString());

    // Get send to contract parameters for add delegation for address
    UniValue params(UniValue::VARR);
    UniValue contractaddress = HexStr(Params().GetConsensus().GetDelegationsAddress(chainActive.Height()));
    UniValue amount = 0;
    UniValue gasLimit = 0;

    if (chainActive.Height() >= Params().GetConsensus().nDelegationsGasFixHeight) {
        gasLimit = request.params.size() > 3 ? request.params[3] : DEFAULT_GAS_LIMIT_OP_SEND;
    } else {
        gasLimit = request.params.size() > 3 ? request.params[3] : DEFAULT_GAS_LIMIT_OP_CREATE;
    }

    UniValue senderaddress = request.params[2];
    CAmount lockAmount;
    if (request.params.size() > 4 && (request.params[4].getValStr() == std::to_string(COIN) || request.params[4].getValStr() == "-1")) lockAmount = -1;
    else lockAmount = request.params.size() > 4 ? AmountFromValue(request.params[4]) : 0;

    // Parse the staker address
    CTxDestination destStaker = DecodeDestination(request.params[0].get_str());
    const CKeyID* pkhStaker = boost::get<CKeyID>(&destStaker);
    if (!pkhStaker) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid contract address for staker. Only P2PK and P2PKH allowed");
    }

    // Parse the staker fee
    int fee = request.params[1].get_int();
    if (fee < 0 || fee > 100)
        throw JSONRPCError(RPC_PARSE_ERROR, "The staker fee need to be between 0 and 100");

    // Parse the sender address
    CTxDestination destSender = DecodeDestination(senderaddress.get_str());
    const CKeyID* pkhSender = boost::get<CKeyID>(&destSender);
    std::string hex_senderaddress = pkhSender->GetReverseHex();
    if (!pkhSender) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid contract sender address. Only P2PK and P2PKH allowed");
    }

    // Get the private key for the sender address
    CKey key;
    CKeyID keyID(*pkhSender);
    if (!pwallet->GetKey(keyID, key)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available for the sender address");
    }

    // Sign the  staker address
    std::vector<unsigned char> PoD;
    std::string hexStaker = pkhStaker->GetReverseHex();
    if (!SignStr::SignMessage(key, hexStaker, PoD))
        throw JSONRPCError(RPC_WALLET_ERROR, "Fail to sign the staker address");

    // Serialize the data
    std::string datahex;
    std::string errorMessage;
    if (!QtumDelegation::BytecodeAdd(hexStaker, fee, PoD, datahex, errorMessage))
        throw JSONRPCError(RPC_TYPE_ERROR, errorMessage);

    // Add the send to contract parameters to the list
    params.push_back(contractaddress);
    params.push_back(datahex);
    params.push_back(amount);
    params.push_back(gasLimit);
    params.push_back(senderaddress);

    auto delegate_ret = SendToContract(*locked_chain, pwallet, params);
    // MilliSleep(3000);

    if (lockAmount != 0 && chainActive.Height() >= Params().GetConsensus().nLydraHeight) {
        Lydra lydraContract;
        std::string mintDatahex;
        lydraContract.getMintDatahex(mintDatahex);

        uint64_t locked_hydra_amount;
        lydraContract.getLockedHydraAmountPerAddress(hex_senderaddress, locked_hydra_amount);

        std::map<CTxDestination, CAmount> balances = pwallet->GetAddressBalances(*locked_chain);

        CAmount amount_to_lock;

        if (lockAmount == -1) {
            amount_to_lock = balances[DecodeDestination(senderaddress.get_str())];
            amount_to_lock -= locked_hydra_amount;
            amount_to_lock -= (nGasPrice * DEFAULT_GAS_LIMIT_OP_CREATE * 2);
        } else {
            if (lockAmount > balances[DecodeDestination(senderaddress.get_str())] - locked_hydra_amount - (nGasPrice * DEFAULT_GAS_LIMIT_OP_CREATE)) {
                amount_to_lock = balances[DecodeDestination(senderaddress.get_str())] - locked_hydra_amount - (nGasPrice * DEFAULT_GAS_LIMIT_OP_CREATE);
            } else {
                amount_to_lock = lockAmount;
            }
        }

        if (amount_to_lock > 0) {
            UniValue lydraParams(UniValue::VARR);
            lydraParams.push_back(HexStr(Params().GetConsensus().lydraAddress));
            lydraParams.push_back(mintDatahex);
            lydraParams.push_back(0);
            lydraParams.push_back(gasLimit);
            lydraParams.push_back(senderaddress);
            auto mint_res = SendToContract(*locked_chain, pwallet, lydraParams, amount_to_lock);
            if (mint_res.isObject()) updateLydraLockedCache(amount_to_lock, hex_senderaddress, true);
        } else {
            if (lockAmount != 0) {
                auto strWarning = strprintf("WARNING: Delegation was initiated, but no LYDRA will be minted, as your free balance is below %s HYDRA!", 
                                                ValueFromAmount((int64_t)(oracleGasPrice * DEFAULT_GAS_LIMIT_OP_CREATE * 2)).getValStr());
                throw JSONRPCError(RPC_WALLET_ERROR, strWarning);
            }
        }
    }

    // Send to contract
    return delegate_ret;
}

UniValue GetJsonSuperStakerConfig(const CSuperStakerInfo& superStaker)
{
    // Fill the json object with information
    UniValue result(UniValue::VOBJ);
    result.pushKV("address", EncodeDestination(CKeyID(superStaker.stakerAddress)));
    result.pushKV("customconfig", superStaker.fCustomConfig);
    if (superStaker.fCustomConfig) {
        result.pushKV("stakingminfee", (int64_t)superStaker.nMinFee);
        result.pushKV("stakingminutxovalue", FormatMoney(superStaker.nMinDelegateUtxo));
        UniValue addressList(UniValue::VARR);
        for (uint160 address : superStaker.delegateAddressList) {
            addressList.push_back(EncodeDestination(CKeyID(address)));
        }
        if (interfaces::WhiteList == superStaker.nDelegateAddressType) {
            result.pushKV("allow", addressList);
        }
        if (interfaces::BlackList == superStaker.nDelegateAddressType) {
            result.pushKV("exclude", addressList);
        }
    }

    return result;
}

static UniValue removesuperstakeraddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            RPCHelpMan{
                "removesuperstakeraddress",
                "\nRemove superstaker address from wallet." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The super staker HYDRA address."},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "address", "Address of the staker."},
                                {RPCResult::Type::BOOL, "customconfig", "Custom configuration exist."},
                                {RPCResult::Type::NUM, "stakingminfee", "Minimum fee for delegate."},
                                {RPCResult::Type::NUM, "stakingminutxovalue", "Minimum UTXO value for delegate."},
                                {
                                    RPCResult::Type::ARR,
                                    "allow",
                                    "List of allowed delegate addresses.",
                                    {
                                        {RPCResult::Type::STR, "address", "The delegate address"},
                                    },
                                },
                                {
                                    RPCResult::Type::ARR,
                                    "exclude",
                                    "List of excluded delegate addresses.",
                                    {
                                        {RPCResult::Type::STR, "address", "The delegate address"},
                                    },
                                },
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("removesuperstakeraddress", "HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd") + HelpExampleRpc("removesuperstakeraddress", "HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd")},
            }
                .ToString());

    // Parse the super staker address
    CTxDestination destStaker = DecodeDestination(request.params[0].get_str());
    const CKeyID* pkhStaker = boost::get<CKeyID>(&destStaker);
    if (!pkhStaker) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address for staker. Only P2PK and P2PKH allowed");
    }

    // Search for super staker
    CSuperStakerInfo superStaker;
    bool found = false;
    for (auto item : pwallet->mapSuperStaker) {
        if (CKeyID(item.second.stakerAddress) == *pkhStaker) {
            superStaker = item.second;
            found = true;
            break;
        }
    }

    if (found) {
        // Update super staker data
        if (!pwallet->RemoveSuperStakerEntry(superStaker.GetHash(), true))
            throw JSONRPCError(RPC_TYPE_ERROR, "Failed to remove the super staker");
    } else {
        throw JSONRPCError(RPC_TYPE_ERROR, "Failed to find super staker!");
    }

    return GetJsonSuperStakerConfig(superStaker);
}

static UniValue addsuperstakeraddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            RPCHelpMan{
                "addsuperstakeraddress",
                "\nAdd superstaker address from wallet." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {
                        "",
                        RPCArg::Type::OBJ,
                        RPCArg::Optional::NO,
                        "",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of the staker"},
                            {"stakingminutxovalue", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The output number"},
                            {"stakingminfee", RPCArg::Type::NUM, RPCArg::Optional::NO, "depends on the value of the 'replaceable' and 'locktime' arguments", "The sequence number"},
                            {
                                "allow",
                                RPCArg::Type::ARR,
                                RPCArg::Optional::OMITTED,
                                "A json array with allow delegate addresses.",
                                {
                                    {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The delegate address"},
                                },
                            },
                            {
                                "exclude",
                                RPCArg::Type::ARR,
                                RPCArg::Optional::OMITTED,
                                "A json array with exclude delegate addresses.",
                                {
                                    {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The delegate address"},
                                },
                            },
                        },
                    },
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "address", "Address of the staker."},
                                {RPCResult::Type::BOOL, "customconfig", "Custom configuration exist."},
                                {RPCResult::Type::NUM, "stakingminfee", "Minimum fee for delegate."},
                                {RPCResult::Type::NUM, "stakingminutxovalue", "Minimum UTXO value for delegate."},
                                {
                                    RPCResult::Type::ARR,
                                    "allow",
                                    "List of allowed delegate addresses.",
                                    {
                                        {RPCResult::Type::STR, "address", "The delegate address"},
                                    },
                                },
                                {
                                    RPCResult::Type::ARR,
                                    "exclude",
                                    "List of excluded delegate addresses.",
                                    {
                                        {RPCResult::Type::STR, "address", "The delegate address"},
                                    },
                                },
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("addsuperstakeraddress", "\"{\\\"address\\\":\\\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\\\",\\\"stakingminutxovalue\\\": \\\"100\\\",\\\"stakingminfee\\\": 10}\"") + HelpExampleCli("addsuperstakeraddress", "\"{\\\"address\\\":\\\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\\\",\\\"stakingminutxovalue\\\": \\\"100\\\",\\\"stakingminfee\\\": 10,\\\"allow\\\":[\\\"HD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\"]}\"") + HelpExampleCli("addsuperstakeraddress", "\"{\\\"address\\\":\\\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\\\",\\\"stakingminutxovalue\\\": \\\"100\\\",\\\"stakingminfee\\\": 10,\\\"exclude\\\":[\\\"HD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\"]}\"") + HelpExampleRpc("addsuperstakeraddress", "\"{\\\"address\\\":\\\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\\\",\\\"stakingminutxovalue\\\": \\\"100\\\",\\\"stakingminfee\\\": 10}\"") + HelpExampleRpc("addsuperstakeraddress", "\"{\\\"address\\\":\\\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\\\",\\\"stakingminutxovalue\\\": \\\"100\\\",\\\"stakingminfee\\\": 10,\\\"allow\\\":[\\\"HD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\"]}\"") + HelpExampleRpc("addsuperstakeraddress", "\"{\\\"address\\\":\\\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\\\",\\\"stakingminutxovalue\\\": \\\"100\\\",\\\"stakingminfee\\\": 10,\\\"exclude\\\":[\\\"HD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\"]}\"")},
            }
                .ToString());

    // Get params for the super staker
    UniValue params(UniValue::VOBJ);
    params = request.params[0].get_obj();

    // Parse the super staker address
    if (!params.exists("address"))
        throw JSONRPCError(RPC_TYPE_ERROR, "The super staker address doesn't exist");
    CTxDestination destStaker = DecodeDestination(params["address"].get_str());
    const CKeyID* pkhStaker = boost::get<CKeyID>(&destStaker);
    if (!pkhStaker) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address for staker. Only P2PK and P2PKH allowed");
    }

    // Parse the staking min utxo value
    if (!params.exists("stakingminutxovalue"))
        throw JSONRPCError(RPC_TYPE_ERROR, "The staking min utxo value doesn't exist");
    CAmount nMinUtxoValue = AmountFromValue(params["stakingminutxovalue"]);
    if (nMinUtxoValue < 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for staking min utxo value");

    // Parse the staking min fee
    if (!params.exists("stakingminfee"))
        throw JSONRPCError(RPC_TYPE_ERROR, "The staking min fee doesn't exist");
    CAmount nMinFee = params["stakingminfee"].get_int();
    if (nMinFee < 0 || nMinFee > 100)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid value for staking min fee");

    // Parse the delegation address lists
    if (params.exists("allow") && params.exists("exclude"))
        throw JSONRPCError(RPC_TYPE_ERROR, "The delegation address lists can be empty, or have either allow list or exclude list");

    // Parse the delegation address lists
    int nDelegateAddressType = interfaces::AcceptAll;
    std::vector<UniValue> addressList;
    if (params.exists("allow")) {
        addressList = params["allow"].get_array().getValues();
        nDelegateAddressType = interfaces::WhiteList;
    } else if (params.exists("exclude")) {
        addressList = params["exclude"].get_array().getValues();
        nDelegateAddressType = interfaces::BlackList;
    }
    std::vector<uint160> delegateAddressList;
    for (UniValue address : addressList) {
        CTxDestination destAddress = DecodeDestination(address.get_str());
        const CKeyID* pkhAddress = boost::get<CKeyID>(&destAddress);
        if (!pkhAddress) {
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address for delegate in allow list or exclude list. Only P2PK and P2PKH allowed");
        }
        delegateAddressList.push_back(uint160(*pkhAddress));
    }

    // Search for super staker
    CSuperStakerInfo superStaker;
    bool found = false;
    for (auto item : pwallet->mapSuperStaker) {
        if (CKeyID(item.second.stakerAddress) == *pkhStaker) {
            superStaker = item.second;
            found = true;
            break;
        }
    }

    if (!found) {
        // Set custom configuration
        superStaker.stakerAddress = uint160(*pkhStaker);
        superStaker.fCustomConfig = true;
        superStaker.nMinFee = nMinFee;
        superStaker.nMinDelegateUtxo = nMinUtxoValue;
        superStaker.nDelegateAddressType = nDelegateAddressType;
        superStaker.delegateAddressList = delegateAddressList;

        // Update super staker data
        if (!pwallet->AddSuperStakerEntry(superStaker))
            throw JSONRPCError(RPC_TYPE_ERROR, "Failed to add the super staker");
    } else {
        throw JSONRPCError(RPC_TYPE_ERROR, "Super staker already exists!");
    }

    return GetJsonSuperStakerConfig(superStaker);
}

static UniValue setsuperstakervaluesforaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            RPCHelpMan{
                "setsuperstakervaluesforaddress",
                "\nUpdate super staker configuration values for address." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {
                        "",
                        RPCArg::Type::OBJ,
                        RPCArg::Optional::NO,
                        "",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The address of the staker"},
                            {"stakingminutxovalue", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The output number"},
                            {"stakingminfee", RPCArg::Type::NUM, RPCArg::Optional::NO, "depends on the value of the 'replaceable' and 'locktime' arguments", "The sequence number"},
                            {
                                "allow",
                                RPCArg::Type::ARR,
                                RPCArg::Optional::OMITTED,
                                "A json array with allow delegate addresses.",
                                {
                                    {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The delegate address"},
                                },
                            },
                            {
                                "exclude",
                                RPCArg::Type::ARR,
                                RPCArg::Optional::OMITTED,
                                "A json array with exclude delegate addresses.",
                                {
                                    {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The delegate address"},
                                },
                            },
                        },
                    },
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "address", "Address of the staker."},
                                {RPCResult::Type::BOOL, "customconfig", "Custom configuration exist."},
                                {RPCResult::Type::NUM, "stakingminfee", "Minimum fee for delegate."},
                                {RPCResult::Type::NUM, "stakingminutxovalue", "Minimum UTXO value for delegate."},
                                {
                                    RPCResult::Type::ARR,
                                    "allow",
                                    "List of allowed delegate addresses.",
                                    {
                                        {RPCResult::Type::STR, "address", "The delegate address"},
                                    },
                                },
                                {
                                    RPCResult::Type::ARR,
                                    "exclude",
                                    "List of excluded delegate addresses.",
                                    {
                                        {RPCResult::Type::STR, "address", "The delegate address"},
                                    },
                                },
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("setsuperstakervaluesforaddress", "\"{\\\"address\\\":\\\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\\\",\\\"stakingminutxovalue\\\": \\\"100\\\",\\\"stakingminfee\\\": 10}\"") + HelpExampleCli("setsuperstakervaluesforaddress", "\"{\\\"address\\\":\\\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\\\",\\\"stakingminutxovalue\\\": \\\"100\\\",\\\"stakingminfee\\\": 10,\\\"allow\\\":[\\\"HD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\"]}\"") + HelpExampleCli("setsuperstakervaluesforaddress", "\"{\\\"address\\\":\\\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\\\",\\\"stakingminutxovalue\\\": \\\"100\\\",\\\"stakingminfee\\\": 10,\\\"exclude\\\":[\\\"HD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\"]}\"") + HelpExampleRpc("setsuperstakervaluesforaddress", "\"{\\\"address\\\":\\\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\\\",\\\"stakingminutxovalue\\\": \\\"100\\\",\\\"stakingminfee\\\": 10}\"") + HelpExampleRpc("setsuperstakervaluesforaddress", "\"{\\\"address\\\":\\\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\\\",\\\"stakingminutxovalue\\\": \\\"100\\\",\\\"stakingminfee\\\": 10,\\\"allow\\\":[\\\"HD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\"]}\"") + HelpExampleRpc("setsuperstakervaluesforaddress", "\"{\\\"address\\\":\\\"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\\\",\\\"stakingminutxovalue\\\": \\\"100\\\",\\\"stakingminfee\\\": 10,\\\"exclude\\\":[\\\"HD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\"]}\"")},
            }
                .ToString());

    // Get params for the super staker
    UniValue params(UniValue::VOBJ);
    params = request.params[0].get_obj();

    // Parse the super staker address
    if (!params.exists("address"))
        throw JSONRPCError(RPC_TYPE_ERROR, "The super staker address doesn't exist");
    CTxDestination destStaker = DecodeDestination(params["address"].get_str());
    const CKeyID* pkhStaker = boost::get<CKeyID>(&destStaker);
    if (!pkhStaker) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address for staker. Only P2PK and P2PKH allowed");
    }

    // Parse the staking min utxo value
    if (!params.exists("stakingminutxovalue"))
        throw JSONRPCError(RPC_TYPE_ERROR, "The staking min utxo value doesn't exist");
    CAmount nMinUtxoValue = AmountFromValue(params["stakingminutxovalue"]);
    if (nMinUtxoValue < 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for staking min utxo value");

    // Parse the staking min fee
    if (!params.exists("stakingminfee"))
        throw JSONRPCError(RPC_TYPE_ERROR, "The staking min fee doesn't exist");
    CAmount nMinFee = params["stakingminfee"].get_int();
    if (nMinFee < 0 || nMinFee > 100)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid value for staking min fee");

    // Parse the delegation address lists
    if (params.exists("allow") && params.exists("exclude"))
        throw JSONRPCError(RPC_TYPE_ERROR, "The delegation address lists can be empty, or have either allow list or exclude list");

    // Parse the delegation address lists
    int nDelegateAddressType = interfaces::AcceptAll;
    std::vector<UniValue> addressList;
    if (params.exists("allow")) {
        addressList = params["allow"].get_array().getValues();
        nDelegateAddressType = interfaces::WhiteList;
    } else if (params.exists("exclude")) {
        addressList = params["exclude"].get_array().getValues();
        nDelegateAddressType = interfaces::BlackList;
    }
    std::vector<uint160> delegateAddressList;
    for (UniValue address : addressList) {
        CTxDestination destAddress = DecodeDestination(address.get_str());
        const CKeyID* pkhAddress = boost::get<CKeyID>(&destAddress);
        if (!pkhAddress) {
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address for delegate in allow list or exclude list. Only P2PK and P2PKH allowed");
        }
        delegateAddressList.push_back(uint160(*pkhAddress));
    }

    // Search for super staker
    CSuperStakerInfo superStaker;
    bool found = false;
    for (auto item : pwallet->mapSuperStaker) {
        if (CKeyID(item.second.stakerAddress) == *pkhStaker) {
            superStaker = item.second;
            found = true;
            break;
        }
    }

    if (found) {
        // Set custom configuration
        superStaker.fCustomConfig = true;
        superStaker.nMinFee = nMinFee;
        superStaker.nMinDelegateUtxo = nMinUtxoValue;
        superStaker.nDelegateAddressType = nDelegateAddressType;
        superStaker.delegateAddressList = delegateAddressList;

        // Update super staker data
        if (!pwallet->AddSuperStakerEntry(superStaker))
            throw JSONRPCError(RPC_TYPE_ERROR, "Failed to update the super staker");
    } else {
        throw JSONRPCError(RPC_TYPE_ERROR, "Failed to find the super staker");
    }

    return GetJsonSuperStakerConfig(superStaker);
}

static UniValue listsuperstakercustomvalues(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            RPCHelpMan{
                "listsuperstakercustomvalues",
                "\nList custom super staker configurations values." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {},
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "address", "Address of the staker."},
                                {RPCResult::Type::BOOL, "customconfig", "Custom configuration exist."},
                                {RPCResult::Type::NUM, "stakingminfee", "Minimum fee for delegate."},
                                {RPCResult::Type::NUM, "stakingminutxovalue", "Minimum UTXO value for delegate."},
                                {
                                    RPCResult::Type::ARR,
                                    "allow",
                                    "List of allowed delegate addresses.",
                                    {
                                        {RPCResult::Type::STR, "address", "The delegate address"},
                                    },
                                },
                                {
                                    RPCResult::Type::ARR,
                                    "exclude",
                                    "List of excluded delegate addresses.",
                                    {
                                        {RPCResult::Type::STR, "address", "The delegate address"},
                                    },
                                },
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("listsuperstakercustomvalues", "") + HelpExampleRpc("listsuperstakercustomvalues", "")},
            }
                .ToString());

    // Search for super stakers
    UniValue result(UniValue::VARR);
    for (auto item : pwallet->mapSuperStaker) {
        CSuperStakerInfo superStaker = item.second;
        if (superStaker.fCustomConfig) {
            result.push_back(GetJsonSuperStakerConfig(superStaker));
        }
    }

    return result;
}

static UniValue listsuperstakervaluesforaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            RPCHelpMan{
                "listsuperstakervaluesforaddress",
                "\nList super staker configuration values for address." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The super staker HYDRA address."},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR, "address", "Address of the staker."},
                                {RPCResult::Type::BOOL, "customconfig", "Custom configuration exist."},
                                {RPCResult::Type::NUM, "stakingminfee", "Minimum fee for delegate."},
                                {RPCResult::Type::NUM, "stakingminutxovalue", "Minimum UTXO value for delegate."},
                                {
                                    RPCResult::Type::ARR,
                                    "allow",
                                    "List of allowed delegate addresses.",
                                    {
                                        {RPCResult::Type::STR, "address", "The delegate address"},
                                    },
                                },
                                {
                                    RPCResult::Type::ARR,
                                    "exclude",
                                    "List of excluded delegate addresses.",
                                    {
                                        {RPCResult::Type::STR, "address", "The delegate address"},
                                    },
                                },
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("listsuperstakervaluesforaddress", "HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd") + HelpExampleRpc("listsuperstakervaluesforaddress", "HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd")},
            }
                .ToString());

    // Parse the super staker address
    CTxDestination destStaker = DecodeDestination(request.params[0].get_str());
    const CKeyID* pkhStaker = boost::get<CKeyID>(&destStaker);
    if (!pkhStaker) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address for staker. Only P2PK and P2PKH allowed");
    }

    // Search for super staker
    CSuperStakerInfo superStaker;
    bool found = false;
    for (auto item : pwallet->mapSuperStaker) {
        if (CKeyID(item.second.stakerAddress) == *pkhStaker) {
            superStaker = item.second;
            found = true;
            break;
        }
    }

    if (!found) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Failed to find the super staker");
    }

    return GetJsonSuperStakerConfig(superStaker);
}

static UniValue removesuperstakervaluesforaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            RPCHelpMan{
                "removesuperstakervaluesforaddress",
                "\nRemove super staker configuration values for address." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The super staker HYDRA address."},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("removesuperstakervaluesforaddress", "HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd") + HelpExampleRpc("removesuperstakervaluesforaddress", "HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd")},
            }
                .ToString());

    // Parse the super staker address
    CTxDestination destStaker = DecodeDestination(request.params[0].get_str());
    const CKeyID* pkhStaker = boost::get<CKeyID>(&destStaker);
    if (!pkhStaker) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address for staker. Only P2PK and P2PKH allowed");
    }

    // Search for super staker
    CSuperStakerInfo superStaker;
    bool found = false;
    for (auto item : pwallet->mapSuperStaker) {
        if (CKeyID(item.second.stakerAddress) == *pkhStaker &&
            item.second.fCustomConfig) {
            superStaker = item.second;
            found = true;
            break;
        }
    }

    if (found) {
        // Remove custom configuration
        superStaker.fCustomConfig = false;
        superStaker.nMinFee = 0;
        superStaker.nMinDelegateUtxo = 0;
        superStaker.nDelegateAddressType = 0;
        superStaker.delegateAddressList.clear();

        // Update super staker data
        if (!pwallet->AddSuperStakerEntry(superStaker))
            throw JSONRPCError(RPC_TYPE_ERROR, "Failed to update the super staker");
    } else {
        throw JSONRPCError(RPC_TYPE_ERROR, "Failed to find the super staker");
    }

    return NullUniValue;
}

static UniValue listaddressgroupings(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            RPCHelpMan{
                "listaddressgroupings",
                "\nLists groups of addresses which have had their common ownership\n"
                "made public by common use as inputs or as the resulting change\n"
                "in past transactions\n",
                {},
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::ARR, "", "",
                            {
                                {RPCResult::Type::ARR, "", "",
                                    {
                                        {RPCResult::Type::STR, "address", "The HYDRA address"},
                                        {RPCResult::Type::STR_AMOUNT, "amount", "The amount in " + CURRENCY_UNIT},
                                        {RPCResult::Type::STR, "label", /* optional */ true, "The label"},
                                    }},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("listaddressgroupings", "") + HelpExampleRpc("listaddressgroupings", "")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    UniValue jsonGroupings(UniValue::VARR);
    std::map<CTxDestination, CAmount> balances = pwallet->GetAddressBalances(*locked_chain);
    for (const std::set<CTxDestination>& grouping : pwallet->GetAddressGroupings()) {
        UniValue jsonGrouping(UniValue::VARR);
        for (const CTxDestination& address : grouping) {
            UniValue addressInfo(UniValue::VARR);
            addressInfo.push_back(EncodeDestination(address));
            addressInfo.push_back(ValueFromAmount(balances[address]));
            {
                if (pwallet->mapAddressBook.find(address) != pwallet->mapAddressBook.end()) {
                    addressInfo.push_back(pwallet->mapAddressBook.find(address)->second.name);
                }
            }
            jsonGrouping.push_back(addressInfo);
        }
        jsonGroupings.push_back(jsonGrouping);
    }
    return jsonGroupings;
}

static UniValue signmessage(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            RPCHelpMan{
                "signmessage",
                "\nSign a message with the private key of an address" +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The HYDRA address to use for the private key."},
                    {"message", RPCArg::Type::STR, RPCArg::Optional::NO, "The message to create a signature of."},
                },
                RPCResult{
                    RPCResult::Type::STR, "signature", "The signature of the message encoded in base 64"},
                RPCExamples{
                    "\nUnlock the wallet for 30 seconds\n" + HelpExampleCli("walletpassphrase", "\"mypassphrase\" 30") +
                    "\nCreate the signature\n" + HelpExampleCli("signmessage", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\" \"my message\"") +
                    "\nVerify the signature\n" + HelpExampleCli("verifymessage", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\" \"signature\" \"my message\"") +
                    "\nAs a JSON-RPC call\n" + HelpExampleRpc("signmessage", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\", \"my message\"")},
            }
                .ToString());

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    std::string strAddress = request.params[0].get_str();
    std::string strMessage = request.params[1].get_str();

    CTxDestination dest = DecodeDestination(strAddress);
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");
    }

    const CKeyID* keyID = boost::get<CKeyID>(&dest);
    if (!keyID) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
    }

    CKey key;
    if (!pwallet->GetKey(*keyID, key)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");
    }

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    std::vector<unsigned char> vchSig;
    if (!key.SignCompact(ss.GetHash(), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    return EncodeBase64(vchSig.data(), vchSig.size());
}

static UniValue getbalanceofaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            RPCHelpMan{
                "getbalanceofaddress",
                "\nReturns the balance of an address",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The HYDRA address that will be used."},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::ARR, "", "",
                            {
                                {RPCResult::Type::ARR, "", "",
                                    {
                                        {RPCResult::Type::STR, "address", "The HYDRA address"},
                                        {RPCResult::Type::STR_AMOUNT, "amount", "The amount in " + CURRENCY_UNIT},
                                        {RPCResult::Type::STR, "label", /* optional */ true, "The label"},
                                    }},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("getbalanceofaddress", "HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC") + HelpExampleRpc("getbalanceofaddress", "HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    std::string strAddress = request.params[0].get_str();
    std::map<CTxDestination, CAmount> balances = pwallet->GetAddressBalances(*locked_chain);
    return ValueFromAmount(balances[DecodeDestination(strAddress)]);
}

static UniValue getreceivedbyaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            RPCHelpMan{
                "getreceivedbyaddress",
                "\nReturns the total amount received by the given address in transactions with at least minconf confirmations.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The HYDRA address for transactions."},
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "Only include transactions confirmed at least this many times."},
                },
                RPCResult{
                    RPCResult::Type::STR_AMOUNT, "amount", "The total amount in " + CURRENCY_UNIT + " received at this address."},
                RPCExamples{
                    "\nThe amount from transactions with at least 1 confirmation\n" + HelpExampleCli("getreceivedbyaddress", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\"") +
                    "\nThe amount including unconfirmed transactions, zero confirmations\n" + HelpExampleCli("getreceivedbyaddress", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\" 0") +
                    "\nThe amount with at least 6 confirmations, very safe\n" + HelpExampleCli("getreceivedbyaddress", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\" 6") +
                    "\nAs a JSON-RPC call\n" + HelpExampleRpc("getreceivedbyaddress", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\", 6")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LockAnnotation lock(::cs_main); // Temporary, for CheckFinalTx below. Removed in upcoming commit.
    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    // Bitcoin address
    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid HYDRA address");
    }
    CScript scriptPubKey = GetScriptForDestination(dest);
    if (!IsMine(*pwallet, scriptPubKey)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Address not found in wallet");
    }

    // Minimum confirmations
    int nMinDepth = 1;
    if (!request.params[1].isNull())
        nMinDepth = request.params[1].get_int();

    // Tally
    CAmount nAmount = 0;
    for (const std::pair<const uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;
        if (wtx.IsCoinBase() || wtx.IsCoinStake() || !CheckFinalTx(*wtx.tx))
            continue;

        for (const CTxOut& txout : wtx.tx->vout)
            if (txout.scriptPubKey == scriptPubKey)
                if (wtx.GetDepthInMainChain(*locked_chain) >= nMinDepth)
                    nAmount += txout.nValue;
    }

    return ValueFromAmount(nAmount);
}


static UniValue getreceivedbylabel(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            RPCHelpMan{
                "getreceivedbylabel",
                "\nReturns the total amount received by addresses with <label> in transactions with at least [minconf] confirmations.\n",
                {
                    {"label", RPCArg::Type::STR, RPCArg::Optional::NO, "The selected label, may be the default label using \"\"."},
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "Only include transactions confirmed at least this many times."},
                },
                RPCResult{
                    RPCResult::Type::STR_AMOUNT, "amount", "The total amount in " + CURRENCY_UNIT + " received for this label."},
                RPCExamples{
                    "\nAmount received by the default label with at least 1 confirmation\n" + HelpExampleCli("getreceivedbylabel", "\"\"") +
                    "\nAmount received at the tabby label including unconfirmed amounts with zero confirmations\n" + HelpExampleCli("getreceivedbylabel", "\"tabby\" 0") +
                    "\nThe amount with at least 6 confirmations\n" + HelpExampleCli("getreceivedbylabel", "\"tabby\" 6") +
                    "\nAs a JSON-RPC call\n" + HelpExampleRpc("getreceivedbylabel", "\"tabby\", 6")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LockAnnotation lock(::cs_main); // Temporary, for CheckFinalTx below. Removed in upcoming commit.
    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    // Minimum confirmations
    int nMinDepth = 1;
    if (!request.params[1].isNull())
        nMinDepth = request.params[1].get_int();

    // Get the set of pub keys assigned to label
    std::string label = LabelFromValue(request.params[0]);
    std::set<CTxDestination> setAddress = pwallet->GetLabelAddresses(label);

    // Tally
    CAmount nAmount = 0;
    for (const std::pair<const uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;
        if (wtx.IsCoinBase() || wtx.IsCoinStake() || !CheckFinalTx(*wtx.tx))
            continue;

        for (const CTxOut& txout : wtx.tx->vout) {
            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*pwallet, address) && setAddress.count(address)) {
                if (wtx.GetDepthInMainChain(*locked_chain) >= nMinDepth)
                    nAmount += txout.nValue;
            }
        }
    }

    return ValueFromAmount(nAmount);
}


static UniValue getbalance(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || (request.params.size() > 3))
        throw std::runtime_error(
            RPCHelpMan{
                "getbalance",
                "\nReturns the total available balance.\n"
                "The available balance is what the wallet considers currently spendable, and is\n"
                "thus affected by options which limit spendability such as -spendzeroconfchange.\n",
                {
                    {"dummy", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Remains for backward compatibility. Must be excluded or set to \"*\"."},
                    {"minconf", RPCArg::Type::NUM, /* default */ "0", "Only include transactions confirmed at least this many times."},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "false", "Also include balance in watch-only addresses (see 'importaddress')"},
                },
                RPCResult{
                    RPCResult::Type::STR_AMOUNT, "amount", "The total amount in " + CURRENCY_UNIT + " received for this wallet."},
                RPCExamples{
                    "\nThe total amount in the wallet with 1 or more confirmations\n" + HelpExampleCli("getbalance", "") +
                    "\nThe total amount in the wallet at least 6 blocks confirmed\n" + HelpExampleCli("getbalance", "\"*\" 6") +
                    "\nAs a JSON-RPC call\n" + HelpExampleRpc("getbalance", "\"*\", 6")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    const UniValue& dummy_value = request.params[0];
    if (!dummy_value.isNull() && dummy_value.get_str() != "*") {
        throw JSONRPCError(RPC_METHOD_DEPRECATED, "dummy first argument must be excluded or set to \"*\".");
    }

    int min_depth = 0;
    if (!request.params[1].isNull()) {
        min_depth = request.params[1].get_int();
    }

    isminefilter filter = ISMINE_SPENDABLE;
    if (!request.params[2].isNull() && request.params[2].get_bool()) {
        filter = filter | ISMINE_WATCH_ONLY;
    }

    return ValueFromAmount(pwallet->GetBalance(filter, min_depth));
}

static UniValue getunconfirmedbalance(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 0)
        throw std::runtime_error(
            RPCHelpMan{
                "getunconfirmedbalance",
                "Returns the server's total unconfirmed balance\n",
                {},
                RPCResults{},
                RPCExamples{""},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    return ValueFromAmount(pwallet->GetUnconfirmedBalance());
}


static UniValue sendmany(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 8) {
        throw std::runtime_error(
            RPCHelpMan{"sendmany",
                "\nSend multiple times. Amounts are double-precision floating point numbers." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"dummy", RPCArg::Type::STR, RPCArg::Optional::NO,
                        "Must be set to \"\" for backwards compatibility.", "\"\""},
                    {
                        "amounts",
                        RPCArg::Type::OBJ,
                        RPCArg::Optional::NO,
                        "A json object with addresses and amounts",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO,
                                "The HYDRA address is the key, the numeric amount (can be string) in " +
                                    CURRENCY_UNIT + " is the value"},
                        },
                    },
                    {"minconf", RPCArg::Type::NUM, /* default */ "1",
                        "Only use the balance confirmed at least this many times."},
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment"},
                    {
                        "subtractfeefrom",
                        RPCArg::Type::ARR,
                        RPCArg::Optional::OMITTED_NAMED_ARG,
                        "A json array with addresses.\n"
                        "                           The fee will be equally deducted from the amount of each selected address.\n"
                        "                           Those recipients will receive less HYDRA than you enter in their corresponding amount field.\n"
                        "                           If no addresses are specified here, the sender pays the fee.",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                                "Subtract fee from this address"},
                        },
                    },
                    {"replaceable", RPCArg::Type::BOOL, /* default */ "fallback to wallet's default",
                        "Allow this transaction to be replaced by a transaction with higher fees via BIP 125"},
                    {"conf_target", RPCArg::Type::NUM, /* default */ "fallback to wallet's default",
                        "Confirmation target (in blocks)"},
                    {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET",
                        "The fee estimate mode, must be one of:\n"
                        "       \"UNSET\"\n"
                        "       \"ECONOMICAL\"\n"
                        "       \"CONSERVATIVE\""},
                },
                RPCResult{
                    RPCResult::Type::STR_HEX, "txid", "The transaction id for the send. Only 1 transaction is created regardless of\n"
                                                      "the number of addresses."},
                RPCExamples{
                    "\nSend two amounts to two different addresses:\n" + HelpExampleCli("sendmany", "\"\" \"{\\\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\\\":0.01,\\\"H353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\"") +
                    "\nSend two amounts to two different addresses setting the confirmation and comment:\n" + HelpExampleCli("sendmany", "\"\" \"{\\\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\\\":0.01,\\\"H353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" 6 \"testing\"") +
                    "\nSend two amounts to two different addresses, subtract fee from amount:\n" + HelpExampleCli("sendmany", "\"\" \"{\\\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\\\":0.01,\\\"H353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" 1 \"\" \"[\\\"HD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\",\\\"H353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\"]\"") +
                    "\nAs a JSON-RPC call\n" + HelpExampleRpc("sendmany", "\"\", {\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\":0.01,\"H353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\":0.02}, 6, \"testing\"")}}
                .ToString());
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (pwallet->GetBroadcastTransactions() && !g_connman) {
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");
    }

    if (!request.params[0].isNull() && !request.params[0].get_str().empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Dummy value must be set to \"\"");
    }
    UniValue sendTo = request.params[1].get_obj();
    int nMinDepth = 1;
    if (!request.params[2].isNull())
        nMinDepth = request.params[2].get_int();

    mapValue_t mapValue;
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        mapValue["comment"] = request.params[3].get_str();

    UniValue subtractFeeFromAmount(UniValue::VARR);
    if (!request.params[4].isNull())
        subtractFeeFromAmount = request.params[4].get_array();

    CCoinControl coin_control;
    if (!request.params[5].isNull()) {
        coin_control.m_signal_bip125_rbf = request.params[5].get_bool();
    }

    if (!request.params[6].isNull()) {
        coin_control.m_confirm_target = ParseConfirmTarget(request.params[6]);
    }

    if (!request.params[7].isNull()) {
        if (!FeeModeFromString(request.params[7].get_str(), coin_control.m_fee_mode)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
    }

    std::set<CTxDestination> destinations;
    std::vector<CRecipient> vecSend;

    CAmount totalAmount = 0;
    std::vector<std::string> keys = sendTo.getKeys();
    for (const std::string& name_ : keys) {
        CTxDestination dest = DecodeDestination(name_);
        if (!IsValidDestination(dest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid HYDRA address: ") + name_);
        }

        if (destinations.count(dest)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + name_);
        }
        destinations.insert(dest);

        CScript scriptPubKey = GetScriptForDestination(dest);
        CAmount nAmount = AmountFromValue(sendTo[name_]);
        if (nAmount <= 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");
        totalAmount += nAmount;

        bool fSubtractFeeFromAmount = false;
        for (unsigned int idx = 0; idx < subtractFeeFromAmount.size(); idx++) {
            const UniValue& addr = subtractFeeFromAmount[idx];
            if (addr.get_str() == name_)
                fSubtractFeeFromAmount = true;
        }

        CRecipient recipient = {scriptPubKey, nAmount, fSubtractFeeFromAmount};
        vecSend.push_back(recipient);
    }

    EnsureWalletIsUnlocked(pwallet);

    // Check funds
    if (totalAmount > pwallet->GetLegacyBalance(ISMINE_SPENDABLE, nMinDepth)) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Wallet has insufficient funds");
    }

    // Shuffle recipient list
    std::shuffle(vecSend.begin(), vecSend.end(), FastRandomContext());

    // Send
    CReserveKey keyChange(pwallet);
    CAmount nFeeRequired = 0;
    int nChangePosRet = -1;
    std::string strFailReason;
    CTransactionRef tx;
    bool fCreated = pwallet->CreateTransaction(*locked_chain, vecSend, tx, keyChange, nFeeRequired, nChangePosRet, strFailReason, coin_control);
    if (!fCreated)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strFailReason);
    CValidationState state;
    if (!pwallet->CommitTransaction(tx, std::move(mapValue), {} /* orderForm */, keyChange, g_connman.get(), state)) {
        strFailReason = strprintf("Transaction commit failed:: %s", FormatStateMessage(state));
        throw JSONRPCError(RPC_WALLET_ERROR, strFailReason);
    }

    return tx->GetHash().GetHex();
}

static UniValue sendmanywithdupes(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 5)
        throw std::runtime_error(
            RPCHelpMan{
                "sendmanywithdupes",
                "\nSend multiple times. Amounts are double-precision floating point numbers. Supports duplicate addresses" +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"dummy", RPCArg::Type::STR, RPCArg::Optional::NO, "Must be set to \"\" for backwards compatibility.", "\"\""},
                    {
                        "amounts",
                        RPCArg::Type::OBJ,
                        RPCArg::Optional::NO,
                        "A json object with addresses and amounts",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The HYDRA address is the key, the numeric amount (can be string) in " + CURRENCY_UNIT + " is the value"},
                        },
                    },
                    {"minconf", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "Ignored dummy value"},
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A comment"},
                    {
                        "subtractfeefrom",
                        RPCArg::Type::ARR,
                        RPCArg::Optional::OMITTED_NAMED_ARG,
                        "A json array with addresses.\n"
                        "                           The fee will be equally deducted from the amount of each selected address.\n"
                        "                           Those recipients will receive less HYDRA than you enter in their corresponding amount field.\n"
                        "                           If no addresses are specified here, the sender pays the fee.",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Subtract fee from this address"},
                        },
                    },
                    {"replaceable", RPCArg::Type::BOOL, /* default */ "wallet default", "Allow this transaction to be replaced by a transaction with higher fees via BIP 125"},
                    {"conf_target", RPCArg::Type::NUM, /* default */ "wallet default", "Confirmation target (in blocks)"},
                    {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
                                                                                "       \"UNSET\"\n"
                                                                                "       \"ECONOMICAL\"\n"
                                                                                "       \"CONSERVATIVE\""},
                },
                RPCResult{
                    RPCResult::Type::STR_HEX, "txid", "The transaction id for the send. Only 1 transaction is created regardless of\n"
                                                      "the number of addresses."},
                RPCExamples{
                    "\nSend two amounts to two different addresses:\n" + HelpExampleCli("sendmanywithdupes", "\"\" \"{\\\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\\\":0.01,\\\"H353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\"") +
                    "\nSend two amounts to two different addresses setting the confirmation and comment:\n" + HelpExampleCli("sendmanywithdupes", "\"\" \"{\\\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\\\":0.01,\\\"H353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" 6 \"testing\"") +
                    "\nSend two amounts to two different addresses, subtract fee from amount:\n" + HelpExampleCli("sendmanywithdupes", "\"\" \"{\\\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\\\":0.01,\\\"H353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" 1 \"\" \"[\\\"HD1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\",\\\"H353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\"]\"") +
                    "\nAs a json rpc call\n" + HelpExampleRpc("sendmanywithdupes", "\"\", \"{\\\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\\\":0.01,\\\"H353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\", 6, \"testing\"")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (pwallet->GetBroadcastTransactions() && !g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    if (!request.params[0].isNull() && !request.params[0].get_str().empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Dummy value must be set to \"\"");
    }

    UniValue sendTo = request.params[1].get_obj();

    mapValue_t mapValue;
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        mapValue["comment"] = request.params[3].get_str();

    UniValue subtractFeeFromAmount(UniValue::VARR);
    if (!request.params[4].isNull())
        subtractFeeFromAmount = request.params[4].get_array();

    CCoinControl coin_control;
    if (!request.params[5].isNull()) {
        coin_control.m_signal_bip125_rbf = request.params[5].get_bool();
    }

    if (!request.params[6].isNull()) {
        coin_control.m_confirm_target = ParseConfirmTarget(request.params[6]);
    }

    if (!request.params[7].isNull()) {
        if (!FeeModeFromString(request.params[7].get_str(), coin_control.m_fee_mode)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
    }

    std::set<CTxDestination> destinations;
    std::vector<CRecipient> vecSend;

    std::vector<std::string> keys = sendTo.getKeys();
    int i = 0;
    for (const std::string& name_ : keys) {
        CTxDestination dest = DecodeDestination(name_);
        if (!IsValidDestination(dest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid HYDRA address: ") + name_);
        }

        destinations.insert(dest);

        CScript scriptPubKey = GetScriptForDestination(dest);
        CAmount nAmount = AmountFromValue(sendTo[i]);
        if (nAmount <= 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");

        bool fSubtractFeeFromAmount = false;
        for (unsigned int idx = 0; idx < subtractFeeFromAmount.size(); idx++) {
            const UniValue& addr = subtractFeeFromAmount[idx];
            if (addr.get_str() == name_)
                fSubtractFeeFromAmount = true;
        }

        CRecipient recipient = {scriptPubKey, nAmount, fSubtractFeeFromAmount};
        vecSend.push_back(recipient);
        i++;
    }

    EnsureWalletIsUnlocked(pwallet);

    // Shuffle recipient list
    std::shuffle(vecSend.begin(), vecSend.end(), FastRandomContext());

    // Send
    CReserveKey keyChange(pwallet);
    CAmount nFeeRequired = 0;
    int nChangePosRet = -1;
    std::string strFailReason;
    CTransactionRef tx;
    bool fCreated = pwallet->CreateTransaction(*locked_chain, vecSend, tx, keyChange, nFeeRequired, nChangePosRet, strFailReason, coin_control);
    if (!fCreated)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strFailReason);
    CValidationState state;
    if (!pwallet->CommitTransaction(tx, std::move(mapValue), {} /* orderForm */, keyChange, g_connman.get(), state)) {
        strFailReason = strprintf("Transaction commit failed:: %s", FormatStateMessage(state));
        throw JSONRPCError(RPC_WALLET_ERROR, strFailReason);
    }

    return tx->GetHash().GetHex();
}

static UniValue addmultisigaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 4) {
        std::string msg =
            RPCHelpMan{
                "addmultisigaddress",
                "\nAdd a nrequired-to-sign multisignature address to the wallet. Requires a new wallet backup.\n"
                "Each key is a HYDRA address or hex-encoded public key.\n"
                "This functionality is only intended for use with non-watchonly addresses.\n"
                "See `importaddress` for watchonly p2sh address support.\n"
                "If 'label' is specified, assign address to that label.\n",
                {
                    {"nrequired", RPCArg::Type::NUM, RPCArg::Optional::NO, "The number of required signatures out of the n keys or addresses."},
                    {
                        "keys",
                        RPCArg::Type::ARR,
                        RPCArg::Optional::NO,
                        "A json array of HYDRA addresses or hex-encoded public keys",
                        {
                            {"key", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "HYDRA address or hex-encoded public key"},
                        },
                    },
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "A label to assign the addresses to."},
                    {"address_type", RPCArg::Type::STR, /* default */ "set by -addresstype", "The address type to use. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "address", "The value of the new multisig address"},
                        {RPCResult::Type::STR_HEX, "redeemScript", "The string value of the hex-encoded redemption script"},
                    }},
                RPCExamples{
                    "\nAdd a multisig address from 2 addresses\n" + HelpExampleCli("addmultisigaddress", "2 \"[\\\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\\\",\\\"H6sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\"]\"") +
                    "\nAs a JSON-RPC call\n" + HelpExampleRpc("addmultisigaddress", "2, \"[\\\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\\\",\\\"H6sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\"]\"")},
            }
                .ToString();
        throw std::runtime_error(msg);
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    std::string label;
    if (!request.params[2].isNull())
        label = LabelFromValue(request.params[2]);

    int required = request.params[0].get_int();

    // Get the public keys
    const UniValue& keys_or_addrs = request.params[1].get_array();
    std::vector<CPubKey> pubkeys;
    for (unsigned int i = 0; i < keys_or_addrs.size(); ++i) {
        if (IsHex(keys_or_addrs[i].get_str()) && (keys_or_addrs[i].get_str().length() == 66 || keys_or_addrs[i].get_str().length() == 130)) {
            pubkeys.push_back(HexToPubKey(keys_or_addrs[i].get_str()));
        } else {
            pubkeys.push_back(AddrToPubKey(pwallet, keys_or_addrs[i].get_str()));
        }
    }

    OutputType output_type = pwallet->m_default_address_type;
    if (!request.params[3].isNull()) {
        if (!ParseOutputType(request.params[3].get_str(), output_type)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", request.params[3].get_str()));
        }
    }

    // Construct using pay-to-script-hash:
    CScript inner;
    CTxDestination dest = AddAndGetMultisigDestination(required, pubkeys, output_type, *pwallet, inner);
    pwallet->SetAddressBook(dest, label, "send");

    UniValue result(UniValue::VOBJ);
    result.pushKV("address", EncodeDestination(dest));
    result.pushKV("redeemScript", HexStr(inner.begin(), inner.end()));
    return result;
}

struct tallyitem {
    CAmount nAmount{0};
    int nConf{std::numeric_limits<int>::max()};
    std::vector<uint256> txids;
    bool fIsWatchonly{false};
    tallyitem()
    {
    }
};

static UniValue ListReceived(interfaces::Chain::Lock& locked_chain, CWallet* const pwallet, const UniValue& params, bool by_label) EXCLUSIVE_LOCKS_REQUIRED(pwallet->cs_wallet)
{
    LockAnnotation lock(::cs_main); // Temporary, for CheckFinalTx below. Removed in upcoming commit.

    // Minimum confirmations
    int nMinDepth = 1;
    if (!params[0].isNull())
        nMinDepth = params[0].get_int();

    // Whether to include empty labels
    bool fIncludeEmpty = false;
    if (!params[1].isNull())
        fIncludeEmpty = params[1].get_bool();

    isminefilter filter = ISMINE_SPENDABLE;
    if (!params[2].isNull())
        if (params[2].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    bool has_filtered_address = false;
    CTxDestination filtered_address = CNoDestination();
    if (!by_label && params.size() > 3) {
        if (!IsValidDestinationString(params[3].get_str())) {
            throw JSONRPCError(RPC_WALLET_ERROR, "address_filter parameter was invalid");
        }
        filtered_address = DecodeDestination(params[3].get_str());
        has_filtered_address = true;
    }

    // Tally
    std::map<CTxDestination, tallyitem> mapTally;
    for (const std::pair<const uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;

        if (wtx.IsCoinBase() || wtx.IsCoinStake() || !CheckFinalTx(*wtx.tx))
            continue;

        int nDepth = wtx.GetDepthInMainChain(locked_chain);
        if (nDepth < nMinDepth)
            continue;

        for (const CTxOut& txout : wtx.tx->vout) {
            CTxDestination address;
            if (!ExtractDestination(txout.scriptPubKey, address))
                continue;

            if (has_filtered_address && !(filtered_address == address)) {
                continue;
            }

            isminefilter mine = IsMine(*pwallet, address);
            if (!(mine & filter))
                continue;

            tallyitem& item = mapTally[address];
            item.nAmount += txout.nValue;
            item.nConf = std::min(item.nConf, nDepth);
            item.txids.push_back(wtx.GetHash());
            if (mine & ISMINE_WATCH_ONLY)
                item.fIsWatchonly = true;
        }
    }

    // Reply
    UniValue ret(UniValue::VARR);
    std::map<std::string, tallyitem> label_tally;

    // Create mapAddressBook iterator
    // If we aren't filtering, go from begin() to end()
    auto start = pwallet->mapAddressBook.begin();
    auto end = pwallet->mapAddressBook.end();
    // If we are filtering, find() the applicable entry
    if (has_filtered_address) {
        start = pwallet->mapAddressBook.find(filtered_address);
        if (start != end) {
            end = std::next(start);
        }
    }

    for (auto item_it = start; item_it != end; ++item_it) {
        const CTxDestination& address = item_it->first;
        const std::string& label = item_it->second.name;
        auto it = mapTally.find(address);
        if (it == mapTally.end() && !fIncludeEmpty)
            continue;

        CAmount nAmount = 0;
        int nConf = std::numeric_limits<int>::max();
        bool fIsWatchonly = false;
        if (it != mapTally.end()) {
            nAmount = (*it).second.nAmount;
            nConf = (*it).second.nConf;
            fIsWatchonly = (*it).second.fIsWatchonly;
        }

        if (by_label) {
            tallyitem& _item = label_tally[label];
            _item.nAmount += nAmount;
            _item.nConf = std::min(_item.nConf, nConf);
            _item.fIsWatchonly = fIsWatchonly;
        } else {
            UniValue obj(UniValue::VOBJ);
            if (fIsWatchonly)
                obj.pushKV("involvesWatchonly", true);
            obj.pushKV("address", EncodeDestination(address));
            obj.pushKV("amount", ValueFromAmount(nAmount));
            obj.pushKV("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf));
            obj.pushKV("label", label);
            UniValue transactions(UniValue::VARR);
            if (it != mapTally.end()) {
                for (const uint256& _item : (*it).second.txids) {
                    transactions.push_back(_item.GetHex());
                }
            }
            obj.pushKV("txids", transactions);
            ret.push_back(obj);
        }
    }

    if (by_label) {
        for (const auto& entry : label_tally) {
            CAmount nAmount = entry.second.nAmount;
            int nConf = entry.second.nConf;
            UniValue obj(UniValue::VOBJ);
            if (entry.second.fIsWatchonly)
                obj.pushKV("involvesWatchonly", true);
            obj.pushKV("amount", ValueFromAmount(nAmount));
            obj.pushKV("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf));
            obj.pushKV("label", entry.first);
            ret.push_back(obj);
        }
    }

    return ret;
}

static UniValue listreceivedbyaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            RPCHelpMan{
                "listreceivedbyaddress",
                "\nList balances by receiving address.\n",
                {
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "The minimum number of confirmations before payments are included."},
                    {"include_empty", RPCArg::Type::BOOL, /* default */ "false", "Whether to include addresses that haven't received any payments."},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "false", "Whether to include watch-only addresses (see 'importaddress')."},
                    {"address_filter", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "If present, only return information on this address."},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::BOOL, "involvesWatchonly", "Only returns true if imported addresses were involved in transaction"},
                                {RPCResult::Type::STR, "address", "The receiving address"},
                                {RPCResult::Type::STR_AMOUNT, "amount", "The total amount in " + CURRENCY_UNIT + " received by the address"},
                                {RPCResult::Type::NUM, "confirmations", "The number of confirmations of the most recent transaction included"},
                                {RPCResult::Type::STR, "label", "The label of the receiving address. The default label is \"\""},
                                {RPCResult::Type::ARR, "txids", "",
                                    {
                                        {RPCResult::Type::STR_HEX, "txid", "The ids of transactions received with the address"},
                                    }},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("listreceivedbyaddress", "") + HelpExampleCli("listreceivedbyaddress", "6 true") + HelpExampleRpc("listreceivedbyaddress", "6, true, true") + HelpExampleRpc("listreceivedbyaddress", "6, true, true, \"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\"")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    return ListReceived(*locked_chain, pwallet, request.params, false);
}

static UniValue listreceivedbylabel(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 3)
        throw std::runtime_error(
            RPCHelpMan{
                "listreceivedbylabel",
                "\nList received transactions by label.\n",
                {
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "The minimum number of confirmations before payments are included."},
                    {"include_empty", RPCArg::Type::BOOL, /* default */ "false", "Whether to include labels that haven't received any payments."},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "false", "Whether to include watch-only addresses (see 'importaddress')."},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::BOOL, "involvesWatchonly", "Only returns true if imported addresses were involved in transaction"},
                                {RPCResult::Type::STR_AMOUNT, "amount", "The total amount received by addresses with this label"},
                                {RPCResult::Type::NUM, "confirmations", "The number of confirmations of the most recent transaction included"},
                                {RPCResult::Type::STR, "label", "The label of the receiving address. The default label is \"\""},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("listreceivedbylabel", "") + HelpExampleCli("listreceivedbylabel", "6 true") + HelpExampleRpc("listreceivedbylabel", "6, true, true")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    return ListReceived(*locked_chain, pwallet, request.params, true);
}

static void MaybePushAddress(UniValue& entry, const CTxDestination& dest)
{
    if (IsValidDestination(dest)) {
        entry.pushKV("address", EncodeDestination(dest));
    }
}

/**
 * List transactions based on the given criteria.
 *
 * @param  pwallet        The wallet.
 * @param  wtx            The wallet transaction.
 * @param  nMinDepth      The minimum confirmation depth.
 * @param  fLong          Whether to include the JSON version of the transaction.
 * @param  ret            The UniValue into which the result is stored.
 * @param  filter_ismine  The "is mine" filter flags.
 * @param  filter_label   Optional label string to filter incoming transactions.
 */
static void ListTransactions(interfaces::Chain::Lock& locked_chain, CWallet* const pwallet, const CWalletTx& wtx, int nMinDepth, bool fLong, UniValue& ret, const isminefilter& filter_ismine, const std::string* filter_label) EXCLUSIVE_LOCKS_REQUIRED(pwallet->cs_wallet)
{
    CAmount nFee;
    std::list<COutputEntry> listReceived;
    std::list<COutputEntry> listSent;

    wtx.GetAmounts(listReceived, listSent, nFee, filter_ismine);

    // Check if the coinstake transactions is mined by the wallet
    if (wtx.IsCoinStake() && listSent.size() > 0 && listReceived.size() > 0) {
        // Condense all of the coinstake inputs and outputs into one output and compute its value
        CAmount amount = wtx.GetCredit(locked_chain, filter_ismine) - wtx.GetDebit(filter_ismine);
        COutputEntry output = *listReceived.begin();
        output.amount = amount;
        listReceived.clear();
        listSent.clear();
        listReceived.push_back(output);
    }

    bool involvesWatchonly = wtx.IsFromMe(ISMINE_WATCH_ONLY);

    // Sent
    if (!filter_label) {
        for (const COutputEntry& s : listSent) {
            UniValue entry(UniValue::VOBJ);
            if (involvesWatchonly || (::IsMine(*pwallet, s.destination) & ISMINE_WATCH_ONLY)) {
                entry.pushKV("involvesWatchonly", true);
            }
            MaybePushAddress(entry, s.destination);
            entry.pushKV("category", "send");
            entry.pushKV("amount", ValueFromAmount(-s.amount));
            if (pwallet->mapAddressBook.count(s.destination)) {
                entry.pushKV("label", pwallet->mapAddressBook[s.destination].name);
            }
            entry.pushKV("vout", s.vout);
            entry.pushKV("fee", ValueFromAmount(-nFee));
            if (fLong)
                WalletTxToJSON(pwallet->chain(), locked_chain, wtx, entry);
            entry.pushKV("abandoned", wtx.isAbandoned());
            ret.push_back(entry);
        }
    }

    // Received
    if (listReceived.size() > 0 && wtx.GetDepthInMainChain(locked_chain) >= nMinDepth) {
        for (const COutputEntry& r : listReceived) {
            std::string label;
            if (pwallet->mapAddressBook.count(r.destination)) {
                label = pwallet->mapAddressBook[r.destination].name;
            }
            if (filter_label && label != *filter_label) {
                continue;
            }
            UniValue entry(UniValue::VOBJ);
            if (involvesWatchonly || (::IsMine(*pwallet, r.destination) & ISMINE_WATCH_ONLY)) {
                entry.pushKV("involvesWatchonly", true);
            }
            MaybePushAddress(entry, r.destination);
            if (wtx.IsCoinBase() || wtx.IsCoinStake()) {
                if (wtx.GetDepthInMainChain(locked_chain) < 1)
                    entry.pushKV("category", "orphan");
                else if (wtx.IsImmature(locked_chain))
                    entry.pushKV("category", "immature");
                else
                    entry.pushKV("category", "generate");
            } else {
                entry.pushKV("category", "receive");
            }
            entry.pushKV("amount", ValueFromAmount(r.amount));
            if (pwallet->mapAddressBook.count(r.destination)) {
                entry.pushKV("label", label);
            }
            entry.pushKV("vout", r.vout);
            if (fLong)
                WalletTxToJSON(pwallet->chain(), locked_chain, wtx, entry);
            ret.push_back(entry);
        }
    }
}

static const std::vector<RPCResult> TransactionDescriptionString()
{
    return {
        {RPCResult::Type::NUM, "confirmations", "The number of confirmations for the transaction. Negative confirmations means the\n"
                                                "transaction conflicted that many blocks ago."},
        {RPCResult::Type::BOOL, "trusted", "Only present if we consider transaction to be trusted and so safe to spend from."},
        {RPCResult::Type::STR_HEX, "blockhash", "The block hash containing the transaction."},
        {RPCResult::Type::NUM, "blockindex", "The index of the transaction in the block that includes it."},
        {RPCResult::Type::NUM_TIME, "blocktime", "The block time expressed in " + UNIX_EPOCH_TIME + "."},
        {RPCResult::Type::STR_HEX, "txid", "The transaction id."},
        {RPCResult::Type::NUM_TIME, "time", "The transaction time expressed in " + UNIX_EPOCH_TIME + "."},
        {RPCResult::Type::NUM_TIME, "timereceived", "The time received expressed in " + UNIX_EPOCH_TIME + "."},
        {RPCResult::Type::STR, "comment", "If a comment is associated with the transaction, only present if not empty."},
        {RPCResult::Type::STR, "bip125-replaceable", "(\"yes|no|unknown\") Whether this transaction could be replaced due to BIP125 (replace-by-fee);\n"
                                                     "may be unknown for unconfirmed transactions not in the mempool"}};
}

UniValue listtransactions(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            RPCHelpMan{
                "listtransactions",
                "\nIf a label name is provided, this will return only incoming transactions paying to addresses with the specified label.\n"
                "\nReturns up to 'count' most recent transactions skipping the first 'from' transactions.\n",
                {
                    {"label", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "If set, should be a valid label name to return only incoming transactions\n"
                                                                                      "              with the specified label, or \"*\" to disable filtering and return all transactions."},
                    {"count", RPCArg::Type::NUM, /* default */ "10", "The number of transactions to return"},
                    {"skip", RPCArg::Type::NUM, /* default */ "0", "The number of transactions to skip"},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "false", "Include transactions to watch-only addresses (see 'importaddress')"},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "", Cat(Cat<std::vector<RPCResult>>({
                                                                                           {RPCResult::Type::STR, "address", "The bitcoin address of the transaction."},
                                                                                           {RPCResult::Type::STR, "category", "The transaction category.\n"
                                                                                                                              "\"send\"                  Transactions sent.\n"
                                                                                                                              "\"receive\"               Non-coinbase transactions received.\n"
                                                                                                                              "\"generate\"              Coinbase transactions received with more than 100 confirmations.\n"
                                                                                                                              "\"immature\"              Coinbase transactions received with 100 or fewer confirmations.\n"
                                                                                                                              "\"orphan\"                Orphaned coinbase transactions received."},
                                                                                           {RPCResult::Type::STR_AMOUNT, "amount", "The amount in " + CURRENCY_UNIT + ". This is negative for the 'send' category, and is positive\n"
                                                                                                                                                                      "for all other categories"},
                                                                                           {RPCResult::Type::STR, "label", "A comment for the address/transaction, if any"},
                                                                                           {RPCResult::Type::NUM, "vout", "the vout value"},
                                                                                           {RPCResult::Type::STR_AMOUNT, "fee", "The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the\n"
                                                                                                                                                                              "'send' category of transactions."},
                                                                                       },
                                                               TransactionDescriptionString()),
                                                           {
                                                               {RPCResult::Type::BOOL, "abandoned", "'true' if the transaction has been abandoned (inputs are respendable). Only available for the \n"
                                                                                                    "'send' category of transactions."},
                                                           })},
                    }},
                RPCExamples{"\nList the most recent 10 transactions in the systems\n" + HelpExampleCli("listtransactions", "") + "\nList transactions 100 to 120\n" + HelpExampleCli("listtransactions", "\"*\" 20 100") + "\nAs a JSON-RPC call\n" + HelpExampleRpc("listtransactions", "\"*\", 20, 100")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    const std::string* filter_label = nullptr;
    if (!request.params[0].isNull() && request.params[0].get_str() != "*") {
        filter_label = &request.params[0].get_str();
        if (filter_label->empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Label argument must be a valid label name or \"*\".");
        }
    }
    int nCount = 10;
    if (!request.params[1].isNull())
        nCount = request.params[1].get_int();
    int nFrom = 0;
    if (!request.params[2].isNull())
        nFrom = request.params[2].get_int();
    isminefilter filter = ISMINE_SPENDABLE;
    if (!request.params[3].isNull())
        if (request.params[3].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    if (nCount < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count");
    if (nFrom < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative from");

    UniValue ret(UniValue::VARR);

    {
        auto locked_chain = pwallet->chain().lock();
        LOCK(pwallet->cs_wallet);

        const CWallet::TxItems& txOrdered = pwallet->wtxOrdered;

        // iterate backwards until we have nCount items to return:
        for (CWallet::TxItems::const_reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it) {
            CWalletTx* const pwtx = (*it).second;
            ListTransactions(*locked_chain, pwallet, *pwtx, 0, true, ret, filter, filter_label);
            if ((int)ret.size() >= (nCount + nFrom)) break;
        }
    }

    // ret is newest to oldest

    if (nFrom > (int)ret.size())
        nFrom = ret.size();
    if ((nFrom + nCount) > (int)ret.size())
        nCount = ret.size() - nFrom;

    std::vector<UniValue> arrTmp = ret.getValues();

    std::vector<UniValue>::iterator first = arrTmp.begin();
    std::advance(first, nFrom);
    std::vector<UniValue>::iterator last = arrTmp.begin();
    std::advance(last, nFrom + nCount);

    if (last != arrTmp.end()) arrTmp.erase(last, arrTmp.end());
    if (first != arrTmp.begin()) arrTmp.erase(arrTmp.begin(), first);

    std::reverse(arrTmp.begin(), arrTmp.end()); // Return oldest to newest

    ret.clear();
    ret.setArray();
    ret.push_backV(arrTmp);

    return ret;
}

static UniValue listsinceblock(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            RPCHelpMan{
                "listsinceblock",
                "\nGet all transactions in blocks since block [blockhash], or all transactions if omitted.\n"
                "If \"blockhash\" is no longer a part of the main chain, transactions from the fork point onward are included.\n"
                "Additionally, if include_removed is set, transactions affecting the wallet which were removed are returned in the \"removed\" array.\n",
                {
                    {"blockhash", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "If set, the block hash to list transactions since, otherwise list all transactions."},
                    {"target_confirmations", RPCArg::Type::NUM, /* default */ "1", "Return the nth block hash from the main chain. e.g. 1 would mean the best block hash. Note: this is not used as a filter, but only affects [lastblock] in the return value"},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "false", "Include transactions to watch-only addresses (see 'importaddress')"},
                    {"include_removed", RPCArg::Type::BOOL, /* default */ "true", "Show transactions that were removed due to a reorg in the \"removed\" array\n"
                                                                                  "                                                           (not guaranteed to work on pruned nodes)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::ARR, "transactions", "",
                            {
                                {RPCResult::Type::OBJ, "", "", Cat(Cat<std::vector<RPCResult>>({
                                                                                                   {RPCResult::Type::BOOL, "involvesWatchonly", "Only returns true if imported addresses were involved in transaction."},
                                                                                                   {RPCResult::Type::STR, "address", "The bitcoin address of the transaction."},
                                                                                                   {RPCResult::Type::STR, "category", "The transaction category.\n"
                                                                                                                                      "\"send\"                  Transactions sent.\n"
                                                                                                                                      "\"receive\"               Non-coinbase transactions received.\n"
                                                                                                                                      "\"generate\"              Coinbase transactions received with more than 100 confirmations.\n"
                                                                                                                                      "\"immature\"              Coinbase transactions received with 100 or fewer confirmations.\n"
                                                                                                                                      "\"orphan\"                Orphaned coinbase transactions received."},
                                                                                                   {RPCResult::Type::STR_AMOUNT, "amount", "The amount in " + CURRENCY_UNIT + ". This is negative for the 'send' category, and is positive\n"
                                                                                                                                                                              "for all other categories"},
                                                                                                   {RPCResult::Type::NUM, "vout", "the vout value"},
                                                                                                   {RPCResult::Type::STR_AMOUNT, "fee", "The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the\n"
                                                                                                                                                                                      "'send' category of transactions."},
                                                                                               },
                                                                       TransactionDescriptionString()),
                                                                   {
                                                                       {RPCResult::Type::BOOL, "abandoned", "'true' if the transaction has been abandoned (inputs are respendable). Only available for the \n"
                                                                                                            "'send' category of transactions."},
                                                                       {RPCResult::Type::STR, "label", "A comment for the address/transaction, if any"},
                                                                       {RPCResult::Type::STR, "to", "If a comment to is associated with the transaction."},
                                                                   })},
                            }},
                        {RPCResult::Type::ARR, "removed", "<structure is the same as \"transactions\" above, only present if include_removed=true>\n"
                                                          "Note: transactions that were re-added in the active chain will appear as-is in this array, and may thus have a positive confirmation count.",
                            {
                                {RPCResult::Type::ELISION, "", ""},
                            }},
                        {RPCResult::Type::STR_HEX, "lastblock", "The hash of the block (target_confirmations-1) from the best block on the main chain. This is typically used to feed back into listsinceblock the next time you call it. So you would generally use a target_confirmations of say 6, so you will be continually re-notified of transactions until they've reached 6 confirmations plus any new ones"},
                    }},
                RPCExamples{HelpExampleCli("listsinceblock", "") + HelpExampleCli("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\" 6") + HelpExampleRpc("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\", 6")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    // The way the 'height' is initialized is just a workaround for the gcc bug #47679 since version 4.6.0.
    Optional<int> height = MakeOptional(false, int()); // Height of the specified block or the common ancestor, if the block provided was in a deactivated chain.
    Optional<int> altheight;                           // Height of the specified block, even if it's in a deactivated chain.
    int target_confirms = 1;
    isminefilter filter = ISMINE_SPENDABLE;

    uint256 blockId;
    if (!request.params[0].isNull() && !request.params[0].get_str().empty()) {
        blockId = ParseHashV(request.params[0], "blockhash");
        height = locked_chain->findFork(blockId, &altheight);
        if (!height) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
    }

    if (!request.params[1].isNull()) {
        target_confirms = request.params[1].get_int();

        if (target_confirms < 1) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
        }
    }

    if (!request.params[2].isNull() && request.params[2].get_bool()) {
        filter = filter | ISMINE_WATCH_ONLY;
    }

    bool include_removed = (request.params[3].isNull() || request.params[3].get_bool());

    const Optional<int> tip_height = locked_chain->getHeight();
    int depth = tip_height && height ? (1 + *tip_height - *height) : -1;

    UniValue transactions(UniValue::VARR);

    for (const std::pair<const uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        CWalletTx tx = pairWtx.second;

        if (depth == -1 || abs(tx.GetDepthInMainChain(*locked_chain)) < depth) {
            ListTransactions(*locked_chain, pwallet, tx, 0, true, transactions, filter, nullptr /* filter_label */);
        }
    }

    // when a reorg'd block is requested, we also list any relevant transactions
    // in the blocks of the chain that was detached
    UniValue removed(UniValue::VARR);
    while (include_removed && altheight && *altheight > *height) {
        CBlock block;
        if (!pwallet->chain().findBlock(blockId, &block) || block.IsNull()) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Can't read block from disk");
        }
        for (const CTransactionRef& tx : block.vtx) {
            auto it = pwallet->mapWallet.find(tx->GetHash());
            if (it != pwallet->mapWallet.end()) {
                // We want all transactions regardless of confirmation count to appear here,
                // even negative confirmation ones, hence the big negative.
                ListTransactions(*locked_chain, pwallet, it->second, -100000000, true, removed, filter, nullptr /* filter_label */);
            }
        }
        blockId = block.hashPrevBlock;
        --*altheight;
    }

    int last_height = tip_height ? *tip_height + 1 - target_confirms : -1;
    uint256 lastblock = last_height >= 0 ? locked_chain->getBlockHash(last_height) : uint256();

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("transactions", transactions);
    if (include_removed) ret.pushKV("removed", removed);
    ret.pushKV("lastblock", lastblock.GetHex());

    return ret;
}

static UniValue gettransaction(const JSONRPCRequest& request_)
{
    // long-poll
    JSONRPCRequest& request = (JSONRPCRequest&)request_;

    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
            RPCHelpMan{
                "gettransaction",
                "\nGet detailed information about in-wallet transaction <txid>\n",
                {
                    {"txid", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction id"},
                    {"include_watchonly", RPCArg::Type::BOOL, /* default */ "false", "Whether to include watch-only addresses in balance calculation and details[]"},
                    {"waitconf", RPCArg::Type::NUM, /* default */ "0", "Wait for enough confirmations before returning."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "", Cat(Cat<std::vector<RPCResult>>({
                                                                                      {RPCResult::Type::STR_AMOUNT, "amount", "The amount in " + CURRENCY_UNIT},
                                                                                      {RPCResult::Type::STR_AMOUNT, "fee", "The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the\n"
                                                                                                                                                                         "'send' category of transactions."},
                                                                                  },
                                                          TransactionDescriptionString()),
                                                      {
                                                          {RPCResult::Type::ARR, "details", "", {
                                                                                                    {RPCResult::Type::OBJ, "", "", {
                                                                                                                                       {RPCResult::Type::STR, "address", "The HYDRA address involved in the transaction."},
                                                                                                                                       {RPCResult::Type::STR, "category", "The transaction category.\n"
                                                                                                                                                                          "\"send\"                  Transactions sent.\n"
                                                                                                                                                                          "\"receive\"               Non-coinbase transactions received.\n"
                                                                                                                                                                          "\"generate\"              Coinbase transactions received with more than 100 confirmations.\n"
                                                                                                                                                                          "\"immature\"              Coinbase transactions received with 100 or fewer confirmations.\n"
                                                                                                                                                                          "\"orphan\"                Orphaned coinbase transactions received."},
                                                                                                                                       {RPCResult::Type::STR_AMOUNT, "amount", "The amount in " + CURRENCY_UNIT},
                                                                                                                                       {RPCResult::Type::STR, "label", "A comment for the address/transaction, if any"},
                                                                                                                                       {RPCResult::Type::NUM, "vout", "the vout value"},
                                                                                                                                       {RPCResult::Type::STR_AMOUNT, "fee", "The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the \n"
                                                                                                                                                                                                                          "'send' category of transactions."},
                                                                                                                                       {RPCResult::Type::BOOL, "abandoned", "'true' if the transaction has been abandoned (inputs are respendable). Only available for the \n"
                                                                                                                                                                            "'send' category of transactions."},
                                                                                                                                   }},
                                                                                                }},
                                                          {RPCResult::Type::STR_HEX, "hex", "Raw data for transaction"},
                                                      })},
                RPCExamples{HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"") + HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\" true") + HelpExampleRpc("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();


    uint256 hash(ParseHashV(request.params[0], "txid"));

    isminefilter filter = ISMINE_SPENDABLE;
    if (!request.params[1].isNull())
        if (request.params[1].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    int waitconf = 0;
    if (request.params.size() > 2) {
        waitconf = request.params[2].get_int();
    }

    bool shouldWaitConf = request.params.size() > 2 && waitconf > 0;

    {
        auto locked_chain = pwallet->chain().lock();
        LOCK(pwallet->cs_wallet);
        if (!pwallet->mapWallet.count(hash))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    }

    CWalletTx* _wtx = nullptr;

    // avoid long-poll if API caller does not specify waitconf
    if (!shouldWaitConf) {
        {
            auto locked_chain = pwallet->chain().lock();
            LOCK(pwallet->cs_wallet);
            _wtx = &pwallet->mapWallet.at(hash);
        }

    } else {
        request.PollStart();
        while (true) {
            {
                auto locked_chain = pwallet->chain().lock();
                LOCK(pwallet->cs_wallet);
                _wtx = &pwallet->mapWallet.at(hash);

                if (_wtx->GetDepthInMainChain(*locked_chain) >= waitconf) {
                    break;
                }
            }

            request.PollPing();

            std::unique_lock<std::mutex> lock(cs_blockchange);
            cond_blockchange.wait_for(lock, std::chrono::milliseconds(300));

            if (!request.PollAlive() || !IsRPCRunning()) {
                return NullUniValue;
            }
        }
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    CWalletTx& wtx = *_wtx;

    UniValue entry(UniValue::VOBJ);

    CAmount nCredit = wtx.GetCredit(*locked_chain, filter);
    CAmount nDebit = wtx.GetDebit(filter);
    CAmount nNet = nCredit - nDebit;
    CAmount nFee = (wtx.IsFromMe(filter) ? wtx.tx->GetValueOut() - nDebit : 0);

    if (wtx.IsCoinStake()) {
        CAmount amount = nNet;
        entry.pushKV("amount", ValueFromAmount(amount));
    } else {
        entry.pushKV("amount", ValueFromAmount(nNet - nFee));
        if (wtx.IsFromMe(filter))
            entry.pushKV("fee", ValueFromAmount(nFee));
    }

    WalletTxToJSON(pwallet->chain(), *locked_chain, wtx, entry);

    UniValue details(UniValue::VARR);
    ListTransactions(*locked_chain, pwallet, wtx, 0, false, details, filter, nullptr /* filter_label */);
    entry.pushKV("details", details);

    std::string strHex = EncodeHexTx(*wtx.tx, RPCSerializationFlags());
    entry.pushKV("hex", strHex);

    return entry;
}

static UniValue abandontransaction(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            RPCHelpMan{
                "abandontransaction",
                "\nMark in-wallet transaction <txid> as abandoned\n"
                "This will mark this transaction and all its in-wallet descendants as abandoned which will allow\n"
                "for their inputs to be respent.  It can be used to replace \"stuck\" or evicted transactions.\n"
                "It only works on transactions which are not included in a block and are not currently in the mempool.\n"
                "It has no effect on transactions which are already abandoned.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("abandontransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"") + HelpExampleRpc("abandontransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")},
            }
                .ToString());
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    uint256 hash(ParseHashV(request.params[0], "txid"));

    if (!pwallet->mapWallet.count(hash)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    }
    if (!pwallet->AbandonTransaction(*locked_chain, hash)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not eligible for abandonment");
    }

    return NullUniValue;
}


static UniValue backupwallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            RPCHelpMan{
                "backupwallet",
                "\nSafely copies current wallet file to destination, which can be a directory or a path with filename.\n",
                {
                    {"destination", RPCArg::Type::STR, RPCArg::Optional::NO, "The destination directory or file"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("backupwallet", "\"backup.dat\"") + HelpExampleRpc("backupwallet", "\"backup.dat\"")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    std::string strDest = request.params[0].get_str();
    if (!pwallet->BackupWallet(strDest)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet backup failed!");
    }

    return NullUniValue;
}


static UniValue keypoolrefill(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            RPCHelpMan{
                "keypoolrefill",
                "\nFills the keypool." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"newsize", RPCArg::Type::NUM, /* default */ "100", "The new keypool size"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("keypoolrefill", "") + HelpExampleRpc("keypoolrefill", "")},
            }
                .ToString());

    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    // 0 is interpreted by TopUpKeyPool() as the default keypool size given by -keypool
    unsigned int kpSize = 0;
    if (!request.params[0].isNull()) {
        if (request.params[0].get_int() < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected valid size.");
        kpSize = (unsigned int)request.params[0].get_int();
    }

    EnsureWalletIsUnlocked(pwallet);
    pwallet->TopUpKeyPool(kpSize);

    if (pwallet->GetKeyPoolSize() < kpSize) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error refreshing keypool.");
    }

    return NullUniValue;
}


static UniValue walletpassphrase(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || (pwallet->IsCrypted() && (request.params.size() < 2 || request.params.size() > 3))) {
        throw std::runtime_error(
            RPCHelpMan{
                "walletpassphrase",
                "\nStores the wallet decryption key in memory for 'timeout' seconds.\n"
                "This is needed prior to performing transactions related to private keys such as sending HYDRA and staking\n"
                "\nNote:\n"
                "Issuing the walletpassphrase command while the wallet is already unlocked will set a new unlock\n"
                "time that overrides the old one.\n",
                {
                    {"passphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The wallet passphrase"},
                    {"timeout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The time to keep the decryption key in seconds; capped at 100000000 (~3 years)."},
                    {"staking only", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Unlock wallet for staking only"},
                },
                RPCResults{},
                RPCExamples{
                    "\nUnlock the wallet for 60 seconds\n" + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 60") +
                    "\nLock the wallet again (before 60 seconds)\n" + HelpExampleCli("walletlock", "") +
                    "\nUnlock the wallet for staking only, for a long time\n" + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 99999999 true") +
                    "\nAs a JSON-RPC call\n" + HelpExampleRpc("walletpassphrase", "\"my pass phrase\", 60")},
            }
                .ToString());
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (request.fHelp)
        return true;

    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrase was called.");
    }

    // Note that the walletpassphrase is stored in request.params[0] which is not mlock()ed
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    strWalletPass = request.params[0].get_str().c_str();

    // Get the timeout
    int64_t nSleepTime = request.params[1].get_int64();
    // Timeout cannot be negative, otherwise it will relock immediately
    if (nSleepTime < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Timeout cannot be negative.");
    }
    // Clamp timeout
    constexpr int64_t MAX_SLEEP_TIME = 100000000; // larger values trigger a macos/libevent bug?
    if (nSleepTime > MAX_SLEEP_TIME) {
        nSleepTime = MAX_SLEEP_TIME;
    }

    if (strWalletPass.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "passphrase can not be empty");
    }

    // Used to restore m_wallet_unlock_staking_only value in case of unlock failure
    bool tmpStakingOnly = pwallet->m_wallet_unlock_staking_only;

    // ppcoin: if user OS account compromised prevent trivial sendmoney commands
    if (request.params.size() > 2)
        pwallet->m_wallet_unlock_staking_only = request.params[2].get_bool();
    else
        pwallet->m_wallet_unlock_staking_only = false;

    if (!pwallet->Unlock(strWalletPass)) {
        pwallet->m_wallet_unlock_staking_only = tmpStakingOnly;
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
    }

    pwallet->TopUpKeyPool();

    pwallet->nRelockTime = GetTime() + nSleepTime;

    // Keep a weak pointer to the wallet so that it is possible to unload the
    // wallet before the following callback is called. If a valid shared pointer
    // is acquired in the callback then the wallet is still loaded.
    std::weak_ptr<CWallet> weak_wallet = wallet;
    RPCRunLater(
        strprintf("lockwallet(%s)", pwallet->GetName()), [weak_wallet] {
            if (auto shared_wallet = weak_wallet.lock()) {
                LOCK(shared_wallet->cs_wallet);
                shared_wallet->Lock();
                shared_wallet->nRelockTime = 0;
            }
        },
        nSleepTime);

    return NullUniValue;
}


static UniValue walletpassphrasechange(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || (pwallet->IsCrypted() && (request.params.size() != 2))) {
        throw std::runtime_error(
            RPCHelpMan{
                "walletpassphrasechange",
                "\nChanges the wallet passphrase from 'oldpassphrase' to 'newpassphrase'.\n",
                {
                    {"oldpassphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The current passphrase"},
                    {"newpassphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The new passphrase"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("walletpassphrasechange", "\"old one\" \"new one\"") + HelpExampleRpc("walletpassphrasechange", "\"old one\", \"new one\"")},
            }
                .ToString());
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrasechange was called.");
    }

    // TODO: get rid of these .c_str() calls by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strOldWalletPass;
    strOldWalletPass.reserve(100);
    strOldWalletPass = request.params[0].get_str().c_str();

    SecureString strNewWalletPass;
    strNewWalletPass.reserve(100);
    strNewWalletPass = request.params[1].get_str().c_str();

    if (strOldWalletPass.empty() || strNewWalletPass.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "passphrase can not be empty");
    }

    if (!pwallet->ChangeWalletPassphrase(strOldWalletPass, strNewWalletPass)) {
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
    }

    return NullUniValue;
}


static UniValue walletlock(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || (pwallet->IsCrypted() && (request.params.size() != 0))) {
        throw std::runtime_error(
            RPCHelpMan{
                "walletlock",
                "\nRemoves the wallet encryption key from memory, locking the wallet.\n"
                "After calling this method, you will need to call walletpassphrase again\n"
                "before being able to call any methods which require the wallet to be unlocked.\n",
                {},
                RPCResults{},
                RPCExamples{
                    "\nSet the passphrase for 2 minutes to perform a transaction\n" + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 120") +
                    "\nPerform a send (requires passphrase set)\n" + HelpExampleCli("sendtoaddress", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\" 1.0") +
                    "\nClear the passphrase since we are done before 2 minutes is up\n" + HelpExampleCli("walletlock", "") +
                    "\nAs a JSON-RPC call\n" + HelpExampleRpc("walletlock", "")},
            }
                .ToString());
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletlock was called.");
    }

    pwallet->Lock();
    pwallet->nRelockTime = 0;

    return NullUniValue;
}


static UniValue encryptwallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || (!pwallet->IsCrypted() && (request.params.size() != 1))) {
        throw std::runtime_error(
            RPCHelpMan{
                "encryptwallet",
                "\nEncrypts the wallet with 'passphrase'. This is for first time encryption.\n"
                "After this, any calls that interact with private keys such as sending or signing \n"
                "will require the passphrase to be set prior the making these calls.\n"
                "Use the walletpassphrase call for this, and then walletlock call.\n"
                "If the wallet is already encrypted, use the walletpassphrasechange call.\n",
                {
                    {"passphrase", RPCArg::Type::STR, RPCArg::Optional::NO, "The pass phrase to encrypt the wallet with. It must be at least 1 character, but should be long."},
                },
                RPCResults{},
                RPCExamples{
                    "\nEncrypt your wallet\n" + HelpExampleCli("encryptwallet", "\"my pass phrase\"") +
                    "\nNow set the passphrase to use the wallet, such as for signing or sending HYDRA\n" + HelpExampleCli("walletpassphrase", "\"my pass phrase\"") +
                    "\nNow we can do something like sign\n" + HelpExampleCli("signmessage", "\"address\" \"test message\"") +
                    "\nNow lock the wallet again by removing the passphrase\n" + HelpExampleCli("walletlock", "") +
                    "\nAs a JSON-RPC call\n" + HelpExampleRpc("encryptwallet", "\"my pass phrase\"")},
            }
                .ToString());
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an encrypted wallet, but encryptwallet was called.");
    }

    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    strWalletPass = request.params[0].get_str().c_str();

    if (strWalletPass.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "passphrase can not be empty");
    }

    if (!pwallet->EncryptWallet(strWalletPass)) {
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: Failed to encrypt the wallet.");
    }

    return "wallet encrypted; The keypool has been flushed and a new HD seed was generated (if you are using HD). You need to make a new backup.";
}

static UniValue reservebalance(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            RPCHelpMan{
                "reservebalance",
                "\nSet reserve amount not participating in network protection."
                "\nIf no parameters provided current setting is printed.\n",
                {
                    {"reserve", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "is true or false to turn balance reserve on or off."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "is a real and rounded to cent."},
                },
                RPCResults{},
                RPCExamples{
                    "\nSet reserve balance to 100\n" + HelpExampleCli("reservebalance", "true 100") +
                    "\nSet reserve balance to 0\n" + HelpExampleCli("reservebalance", "false") +
                    "\nGet reserve balance\n" + HelpExampleCli("reservebalance", "")},
            }
                .ToString());


    if (request.params.size() > 0) {
        bool fReserve = request.params[0].get_bool();
        if (fReserve) {
            if (request.params.size() == 1)
                throw std::runtime_error("must provide amount to reserve balance.\n");
            int64_t nAmount = AmountFromValue(request.params[1]);
            nAmount = (nAmount / CENT) * CENT; // round to cent
            if (nAmount < 0)
                throw std::runtime_error("amount cannot be negative.\n");
            pwallet->m_reserve_balance = nAmount;
        } else {
            if (request.params.size() > 1)
                throw std::runtime_error("cannot specify amount to turn off reserve.\n");
            pwallet->m_reserve_balance = 0;
        }
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("reserve", (pwallet->m_reserve_balance > 0));
    result.pushKV("amount", ValueFromAmount(pwallet->m_reserve_balance));
    return result;
}

static UniValue lockunspent(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            RPCHelpMan{
                "lockunspent",
                "\nUpdates list of temporarily unspendable outputs.\n"
                "Temporarily lock (unlock=false) or unlock (unlock=true) specified transaction outputs.\n"
                "If no transaction outputs are specified when unlocking then all current locked transaction outputs are unlocked.\n"
                "A locked transaction output will not be chosen by automatic coin selection, when spending HYDRA.\n"
                "Locks are stored in memory only. Nodes start with zero locked outputs, and the locked output list\n"
                "is always cleared (by virtue of process exit) when a node stops or fails.\n"
                "Also see the listunspent call\n",
                {
                    {"unlock", RPCArg::Type::BOOL, RPCArg::Optional::NO, "Whether to unlock (true) or lock (false) the specified transactions"},
                    {
                        "transactions",
                        RPCArg::Type::ARR,
                        /* default */ "empty array",
                        "A json array of objects. Each object the txid (string) vout (numeric).",
                        {
                            {
                                "",
                                RPCArg::Type::OBJ,
                                RPCArg::Optional::OMITTED,
                                "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                },
                            },
                        },
                    },
                },
                RPCResult{
                    RPCResult::Type::BOOL, "", "Whether the command was successful or not"},
                RPCExamples{
                    "\nList the unspent transactions\n" + HelpExampleCli("listunspent", "") +
                    "\nLock an unspent transaction\n" + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
                    "\nList the locked transactions\n" + HelpExampleCli("listlockunspent", "") +
                    "\nUnlock the transaction again\n" + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
                    "\nAs a JSON-RPC call\n" + HelpExampleRpc("lockunspent", "false, \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    RPCTypeCheckArgument(request.params[0], UniValue::VBOOL);

    bool fUnlock = request.params[0].get_bool();

    if (request.params[1].isNull()) {
        if (fUnlock)
            pwallet->UnlockAllCoins();
        return true;
    }

    RPCTypeCheckArgument(request.params[1], UniValue::VARR);

    const UniValue& output_params = request.params[1];

    // Create and validate the COutPoints first.

    std::vector<COutPoint> outputs;
    outputs.reserve(output_params.size());

    for (unsigned int idx = 0; idx < output_params.size(); idx++) {
        const UniValue& o = output_params[idx].get_obj();

        RPCTypeCheckObj(o,
            {
                {"txid", UniValueType(UniValue::VSTR)},
                {"vout", UniValueType(UniValue::VNUM)},
            });

        const uint256 txid(ParseHashO(o, "txid"));
        const int nOutput = find_value(o, "vout").get_int();
        if (nOutput < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");
        }

        const COutPoint outpt(txid, nOutput);

        const auto it = pwallet->mapWallet.find(outpt.hash);
        if (it == pwallet->mapWallet.end()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, unknown transaction");
        }

        const CWalletTx& trans = it->second;

        if (outpt.n >= trans.tx->vout.size()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout index out of bounds");
        }

        if (pwallet->IsSpent(*locked_chain, outpt.hash, outpt.n)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected unspent output");
        }

        const bool is_locked = pwallet->IsLockedCoin(outpt.hash, outpt.n);

        if (fUnlock && !is_locked) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected locked output");
        }

        if (!fUnlock && is_locked) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, output already locked");
        }

        outputs.push_back(outpt);
    }

    // Atomically set (un)locked status for the outputs.
    for (const COutPoint& outpt : outputs) {
        if (fUnlock)
            pwallet->UnlockCoin(outpt);
        else
            pwallet->LockCoin(outpt);
    }

    return true;
}

static UniValue listlockunspent(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 0)
        throw std::runtime_error(
            RPCHelpMan{
                "listlockunspent",
                "\nReturns list of temporarily unspendable outputs.\n"
                "See the lockunspent call to lock and unlock transactions for spending.\n",
                {},
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "txid", "The transaction id locked"},
                                {RPCResult::Type::NUM, "vout", "The vout value"},
                            }},
                    }},
                RPCExamples{
                    "\nList the unspent transactions\n" + HelpExampleCli("listunspent", "") +
                    "\nLock an unspent transaction\n" + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
                    "\nList the locked transactions\n" + HelpExampleCli("listlockunspent", "") +
                    "\nUnlock the transaction again\n" + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
                    "\nAs a JSON-RPC call\n" + HelpExampleRpc("listlockunspent", "")},
            }
                .ToString());

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    std::vector<COutPoint> vOutpts;
    pwallet->ListLockedCoins(vOutpts);

    UniValue ret(UniValue::VARR);

    for (const COutPoint& outpt : vOutpts) {
        UniValue o(UniValue::VOBJ);

        o.pushKV("txid", outpt.hash.GetHex());
        o.pushKV("vout", (int)outpt.n);
        ret.push_back(o);
    }

    return ret;
}

static UniValue settxfee(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 1) {
        throw std::runtime_error(
            RPCHelpMan{
                "settxfee",
                "\nSet the transaction fee per kB for this wallet. Overrides the global -paytxfee command line parameter.\n",
                {
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The transaction fee in " + CURRENCY_UNIT + "/kB"},
                },
                RPCResult{
                    RPCResult::Type::BOOL, "", "Returns true if successful"},
                RPCExamples{
                    HelpExampleCli("settxfee", "0.00001") + HelpExampleRpc("settxfee", "0.00001")},
            }
                .ToString());
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    CAmount nAmount = AmountFromValue(request.params[0]);
    CFeeRate tx_fee_rate(nAmount, 1000);
    if (tx_fee_rate == 0) {
        // automatic selection
    } else if (tx_fee_rate < ::minRelayTxFee) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("txfee cannot be less than min relay tx fee (%s)", ::minRelayTxFee.ToString()));
    } else if (tx_fee_rate < pwallet->m_min_fee) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("txfee cannot be less than wallet min fee (%s)", pwallet->m_min_fee.ToString()));
    }

    pwallet->m_pay_tx_fee = tx_fee_rate;
    return true;
}

static UniValue getwalletinfo(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            RPCHelpMan{
                "getwalletinfo",
                "Returns an object containing various wallet state info.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ,
                    "",
                    "",
                    {{
                        {RPCResult::Type::STR, "walletname", "the wallet name"},
                        {RPCResult::Type::NUM, "walletversion", "the wallet version"},
                        {RPCResult::Type::STR_AMOUNT, "balance", "DEPRECATED. Identical to getbalances().mine.trusted"},
                        {RPCResult::Type::STR_AMOUNT, "stake", "DEPRECATED. the total stake balance of the wallet in " + CURRENCY_UNIT},
                        {RPCResult::Type::STR_AMOUNT, "unconfirmed_balance", "DEPRECATED. Identical to getbalances().mine.untrusted_pending"},
                        {RPCResult::Type::STR_AMOUNT, "immature_balance", "DEPRECATED. Identical to getbalances().mine.immature"},
                        {RPCResult::Type::NUM, "txcount", "the total number of transactions in the wallet"},
                        {RPCResult::Type::NUM_TIME, "keypoololdest", "the " + UNIX_EPOCH_TIME + " of the oldest pre-generated key in the key pool"},
                        {RPCResult::Type::NUM, "keypoolsize", "how many new keys are pre-generated (only counts external keys)"},
                        {RPCResult::Type::NUM, "keypoolsize_hd_internal", "how many new keys are pre-generated for internal use (used for change outputs, only appears if the wallet is using this feature, otherwise external keys are used)"},
                        {RPCResult::Type::NUM_TIME, "unlocked_until", "the " + UNIX_EPOCH_TIME + " until which the wallet is unlocked for transfers, or 0 if the wallet is locked"},
                        {RPCResult::Type::STR_AMOUNT, "paytxfee", "the transaction fee configuration, set in " + CURRENCY_UNIT + "/kB"},
                        {RPCResult::Type::STR_HEX, "hdseedid", /* optional */ true, "the Hash160 of the HD seed (only present when HD is enabled)"},
                        {RPCResult::Type::BOOL, "private_keys_enabled", "false if privatekeys are disabled for this wallet (enforced watch-only wallet)"},
                    }},
                },
                RPCExamples{
                    HelpExampleCli("getwalletinfo", "") + HelpExampleRpc("getwalletinfo", "")},
            }
                .ToString());

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    UniValue obj(UniValue::VOBJ);

    size_t kpExternalSize = pwallet->KeypoolCountExternalKeys();
    obj.pushKV("walletname", pwallet->GetName());
    obj.pushKV("walletversion", pwallet->GetVersion());
    obj.pushKV("balance", ValueFromAmount(pwallet->GetBalance()));
    obj.pushKV("stake", ValueFromAmount(pwallet->GetStake()));
    obj.pushKV("unconfirmed_balance", ValueFromAmount(pwallet->GetUnconfirmedBalance()));
    obj.pushKV("immature_balance", ValueFromAmount(pwallet->GetImmatureBalance()));
    obj.pushKV("txcount", (int)pwallet->mapWallet.size());
    obj.pushKV("keypoololdest", pwallet->GetOldestKeyPoolTime());
    obj.pushKV("keypoolsize", (int64_t)kpExternalSize);
    CKeyID seed_id = pwallet->GetHDChain().seed_id;
    if (pwallet->CanSupportFeature(FEATURE_HD_SPLIT)) {
        obj.pushKV("keypoolsize_hd_internal", (int64_t)(pwallet->GetKeyPoolSize() - kpExternalSize));
    }
    if (pwallet->IsCrypted()) {
        obj.pushKV("unlocked_until", pwallet->nRelockTime);
    }
    obj.pushKV("paytxfee", ValueFromAmount(pwallet->m_pay_tx_fee.GetFeePerK()));
    if (!seed_id.IsNull()) {
        obj.pushKV("hdseedid", seed_id.GetHex());
    }
    obj.pushKV("private_keys_enabled", !pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS));
    return obj;
}

static UniValue listwalletdir(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0) {
        throw std::runtime_error(
            RPCHelpMan{
                "listwalletdir",
                "Returns a list of wallets in the wallet directory.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::ARR, "wallets", "",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                    {
                                        {RPCResult::Type::STR, "name", "The wallet name"},
                                    }},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("listwalletdir", "") + HelpExampleRpc("listwalletdir", "")},
            }
                .ToString());
    }

    UniValue wallets(UniValue::VARR);
    for (const auto& path : ListWalletDir()) {
        UniValue wallet(UniValue::VOBJ);
        wallet.pushKV("name", path.string());
        wallets.push_back(wallet);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("wallets", wallets);
    return result;
}

static UniValue listwallets(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            RPCHelpMan{
                "listwallets",
                "Returns a list of currently loaded wallets.\n"
                "For full information on the wallet, use \"getwalletinfo\"\n",
                {},
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::STR, "walletname", "the wallet name"},
                    }},
                RPCExamples{
                    HelpExampleCli("listwallets", "") + HelpExampleRpc("listwallets", "")},
            }
                .ToString());

    UniValue obj(UniValue::VARR);

    for (const std::shared_ptr<CWallet>& wallet : GetWallets()) {
        if (!EnsureWalletIsAvailable(wallet.get(), request.fHelp)) {
            return NullUniValue;
        }

        LOCK(wallet->cs_wallet);

        obj.push_back(wallet->GetName());
    }

    return obj;
}

static UniValue loadwallet(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            RPCHelpMan{
                "loadwallet",
                "\nLoads a wallet from a wallet file or directory."
                "\nNote that all wallet command-line options used when starting hydrad will be"
                "\napplied to the new wallet (eg -zapwallettxes, upgradewallet, rescan, etc).\n",
                {
                    {"filename", RPCArg::Type::STR, RPCArg::Optional::NO, "The wallet directory or .dat file."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "name", "The wallet name if loaded successfully."},
                        {RPCResult::Type::STR, "warning", "Warning message if wallet was not loaded cleanly."},
                    }},
                RPCExamples{
                    HelpExampleCli("loadwallet", "\"test.dat\"") + HelpExampleRpc("loadwallet", "\"test.dat\"")},
            }
                .ToString());

    WalletLocation location(request.params[0].get_str());

    if (!location.Exists()) {
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Wallet " + location.GetName() + " not found.");
    } else if (fs::is_directory(location.GetPath())) {
        // The given filename is a directory. Check that there's a wallet.dat file.
        fs::path wallet_dat_file = location.GetPath() / "wallet.dat";
        if (fs::symlink_status(wallet_dat_file).type() == fs::file_not_found) {
            throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Directory " + location.GetName() + " does not contain a wallet.dat file.");
        }
    }

    std::string error, warning;
    std::shared_ptr<CWallet> const wallet = LoadWallet(*g_rpc_interfaces->chain, location, error, warning);
    if (!wallet) throw JSONRPCError(RPC_WALLET_ERROR, error);


    UniValue obj(UniValue::VOBJ);
    obj.pushKV("name", wallet->GetName());
    obj.pushKV("warning", warning);

    return obj;
}

static UniValue createwallet(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3) {
        throw std::runtime_error(
            RPCHelpMan{
                "createwallet",
                "\nCreates and loads a new wallet.\n",
                {
                    {"wallet_name", RPCArg::Type::STR, RPCArg::Optional::NO, "The name for the new wallet. If this is a path, the wallet will be created at the path location."},
                    {"disable_private_keys", RPCArg::Type::BOOL, /* default */ "false", "Disable the possibility of private keys (only watchonlys are possible in this mode)."},
                    {"blank", RPCArg::Type::BOOL, /* default */ "false", "Create a blank wallet. A blank wallet has no keys or HD seed. One can be set using sethdseed."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "name", "The wallet name if created successfully. If the wallet was created using a full path, the wallet_name will be the full path."},
                        {RPCResult::Type::STR, "warning", "Warning message if wallet was not loaded cleanly."},
                    }},
                RPCExamples{
                    HelpExampleCli("createwallet", "\"testwallet\"") + HelpExampleRpc("createwallet", "\"testwallet\"")},
            }
                .ToString());
    }
    std::string error;
    std::string warning;
    CScheduler scheduler;

    uint64_t flags = 0;
    if (!request.params[1].isNull() && request.params[1].get_bool()) {
        flags |= WALLET_FLAG_DISABLE_PRIVATE_KEYS;
    }

    if (!request.params[2].isNull() && request.params[2].get_bool()) {
        flags |= WALLET_FLAG_BLANK_WALLET;
    }

    WalletLocation location(request.params[0].get_str());
    if (location.Exists()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet " + location.GetName() + " already exists.");
    }

    // Wallet::Verify will check if we're trying to create a wallet with a duplication name.
    if (!CWallet::Verify(*g_rpc_interfaces->chain, location, false, error, warning)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet file verification failed: " + error);
    }

    std::shared_ptr<CWallet> const wallet = CWallet::CreateWalletFromFile(*g_rpc_interfaces->chain, location, flags);
    if (!wallet) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet creation failed.");
    }
    AddWallet(wallet);

    wallet->postInitProcess(scheduler);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("name", wallet->GetName());
    obj.pushKV("warning", warning);

    return obj;
}

static UniValue unloadwallet(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1) {
        throw std::runtime_error(
            RPCHelpMan{
                "unloadwallet",
                "Unloads the wallet referenced by the request endpoint otherwise unloads the wallet specified in the argument.\n"
                "Specifying the wallet name on a wallet endpoint is invalid.",
                {
                    {"wallet_name", RPCArg::Type::STR, /* default */ "the wallet name from the RPC request", "The name of the wallet to unload."},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("unloadwallet", "wallet_name") + HelpExampleRpc("unloadwallet", "wallet_name")},
            }
                .ToString());
    }

    std::string wallet_name;
    if (GetWalletNameFromJSONRPCRequest(request, wallet_name)) {
        if (!request.params[0].isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot unload the requested wallet");
        }
    } else {
        wallet_name = request.params[0].get_str();
    }

    std::shared_ptr<CWallet> wallet = GetWallet(wallet_name);
    if (!wallet) {
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Requested wallet does not exist or is not loaded");
    }

    // Release the "main" shared pointer and prevent further notifications.
    // Note that any attempt to load the same wallet would fail until the wallet
    // is destroyed (see CheckUniqueFileid).
    if (!RemoveWallet(wallet)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Requested wallet already unloaded");
    }

    UnloadWallet(std::move(wallet));

    return NullUniValue;
}

static UniValue resendwallettransactions(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            RPCHelpMan{
                "resendwallettransactions",
                "Immediately re-broadcast unconfirmed wallet transactions to all peers.\n"
                "Intended only for testing; the wallet code periodically re-broadcasts\n"
                "automatically.\n"
                "Returns an RPC error if -walletbroadcast is set to false.\n"
                "Returns array of transaction ids that were re-broadcast.\n",
                {},
                RPCResults{},
                RPCExamples{""},
            }
                .ToString());

    if (!g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (!pwallet->GetBroadcastTransactions()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet transaction broadcasting is disabled with -walletbroadcast");
    }

    std::vector<uint256> txids = pwallet->ResendWalletTransactionsBefore(*locked_chain, GetTime(), g_connman.get());
    UniValue result(UniValue::VARR);
    for (const uint256& txid : txids) {
        result.push_back(txid.ToString());
    }
    return result;
}

static UniValue listunspent(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 5)
        throw std::runtime_error(
            RPCHelpMan{
                "listunspent",
                "\nReturns array of unspent transaction outputs\n"
                "with between minconf and maxconf (inclusive) confirmations.\n"
                "Optionally filter to only include txouts paid to specified addresses.\n",
                {
                    {"minconf", RPCArg::Type::NUM, /* default */ "1", "The minimum confirmations to filter"},
                    {"maxconf", RPCArg::Type::NUM, /* default */ "9999999", "The maximum confirmations to filter"},
                    {
                        "addresses",
                        RPCArg::Type::ARR,
                        /* default */ "empty array",
                        "A json array of HYDRA addresses to filter",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "HYDRA address"},
                        },
                    },
                    {"include_unsafe", RPCArg::Type::BOOL, /* default */ "true", "Include outputs that are not safe to spend\n"
                                                                                 "                  See description of \"safe\" attribute below."},
                    {"query_options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "JSON with query options",
                        {
                            {"minimumAmount", RPCArg::Type::AMOUNT, /* default */ "0", "Minimum value of each UTXO in " + CURRENCY_UNIT + ""},
                            {"maximumAmount", RPCArg::Type::AMOUNT, /* default */ "unlimited", "Maximum value of each UTXO in " + CURRENCY_UNIT + ""},
                            {"maximumCount", RPCArg::Type::NUM, /* default */ "unlimited", "Maximum number of UTXOs"},
                            {"minimumSumAmount", RPCArg::Type::AMOUNT, /* default */ "unlimited", "Minimum sum value of all UTXOs in " + CURRENCY_UNIT + ""},
                        },
                        "query_options"},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "txid", "the transaction id"},
                                {RPCResult::Type::NUM, "vout", "the vout value"},
                                {RPCResult::Type::STR, "address", "the HYDRA address"},
                                {RPCResult::Type::STR, "label", "The associated label, or \"\" for the default label"},
                                {RPCResult::Type::STR, "scriptPubKey", "the script key"},
                                {RPCResult::Type::STR_AMOUNT, "amount", "the transaction output amount in " + CURRENCY_UNIT},
                                {RPCResult::Type::NUM, "confirmations", "The number of confirmations"},
                                {RPCResult::Type::STR_HEX, "redeemScript", "The redeemScript if scriptPubKey is P2SH"},
                                {RPCResult::Type::STR, "witnessScript", "witnessScript if the scriptPubKey is P2WSH or P2SH-P2WSH"},
                                {RPCResult::Type::BOOL, "spendable", "Whether we have the private keys to spend this output"},
                                {RPCResult::Type::BOOL, "solvable", "Whether we know how to spend this output, ignoring the lack of keys"},
                                {RPCResult::Type::STR, "desc", "(only when solvable) A descriptor for spending this output"},
                                {RPCResult::Type::BOOL, "safe", "Whether this output is considered safe to spend. Unconfirmed transactions"
                                                                "from outside keys and unconfirmed replacement transactions are considered unsafe\n"
                                                                "and are not eligible for spending by fundrawtransaction and sendtoaddress."},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("listunspent", "") + HelpExampleCli("listunspent", "6 9999999 \"[\\\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\\\",\\\"H6sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\"]\"") + HelpExampleRpc("listunspent", "6, 9999999 \"[\\\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\\\",\\\"H6sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\"]\"") + HelpExampleCli("listunspent", "6 9999999 '[]' true '{ \"minimumAmount\": 0.005 }'") + HelpExampleRpc("listunspent", "6, 9999999, [] , true, { \"minimumAmount\": 0.005 } ")},
            }
                .ToString());

    int nMinDepth = 1;
    if (!request.params[0].isNull()) {
        RPCTypeCheckArgument(request.params[0], UniValue::VNUM);
        nMinDepth = request.params[0].get_int();
    }

    int nMaxDepth = 9999999;
    if (!request.params[1].isNull()) {
        RPCTypeCheckArgument(request.params[1], UniValue::VNUM);
        nMaxDepth = request.params[1].get_int();
    }

    std::set<CTxDestination> destinations;
    if (!request.params[2].isNull()) {
        RPCTypeCheckArgument(request.params[2], UniValue::VARR);
        UniValue inputs = request.params[2].get_array();
        for (unsigned int idx = 0; idx < inputs.size(); idx++) {
            const UniValue& input = inputs[idx];
            CTxDestination dest = DecodeDestination(input.get_str());
            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid HYDRA address: ") + input.get_str());
            }
            if (!destinations.insert(dest).second) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + input.get_str());
            }
        }
    }

    bool include_unsafe = true;
    if (!request.params[3].isNull()) {
        RPCTypeCheckArgument(request.params[3], UniValue::VBOOL);
        include_unsafe = request.params[3].get_bool();
    }

    CAmount nMinimumAmount = 0;
    CAmount nMaximumAmount = MAX_MONEY;
    CAmount nMinimumSumAmount = MAX_MONEY;
    uint64_t nMaximumCount = 0;

    if (!request.params[4].isNull()) {
        const UniValue& options = request.params[4].get_obj();

        if (options.exists("minimumAmount"))
            nMinimumAmount = AmountFromValue(options["minimumAmount"]);

        if (options.exists("maximumAmount"))
            nMaximumAmount = AmountFromValue(options["maximumAmount"]);

        if (options.exists("minimumSumAmount"))
            nMinimumSumAmount = AmountFromValue(options["minimumSumAmount"]);

        if (options.exists("maximumCount"))
            nMaximumCount = options["maximumCount"].get_int64();
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    UniValue results(UniValue::VARR);
    std::vector<COutput> vecOutputs;
    {
        auto locked_chain = pwallet->chain().lock();
        LOCK(pwallet->cs_wallet);
        pwallet->AvailableCoins(*locked_chain, vecOutputs, !include_unsafe, nullptr, nMinimumAmount, nMaximumAmount, nMinimumSumAmount, nMaximumCount, nMinDepth, nMaxDepth);
    }

    LOCK(pwallet->cs_wallet);

    for (const COutput& out : vecOutputs) {
        CTxDestination address;
        const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
        bool fValidAddress = ExtractDestination(scriptPubKey, address);

        if (destinations.size() && (!fValidAddress || !destinations.count(address)))
            continue;

        UniValue entry(UniValue::VOBJ);
        entry.pushKV("txid", out.tx->GetHash().GetHex());
        entry.pushKV("vout", out.i);

        if (fValidAddress) {
            entry.pushKV("address", EncodeDestination(address));

            auto i = pwallet->mapAddressBook.find(address);
            if (i != pwallet->mapAddressBook.end()) {
                entry.pushKV("label", i->second.name);
            }

            if (scriptPubKey.IsPayToScriptHash()) {
                const CScriptID& hash = boost::get<CScriptID>(address);
                CScript redeemScript;
                if (pwallet->GetCScript(hash, redeemScript)) {
                    entry.pushKV("redeemScript", HexStr(redeemScript.begin(), redeemScript.end()));
                    // Now check if the redeemScript is actually a P2WSH script
                    CTxDestination witness_destination;
                    if (redeemScript.IsPayToWitnessScriptHash()) {
                        bool extracted = ExtractDestination(redeemScript, witness_destination);
                        assert(extracted);
                        // Also return the witness script
                        const WitnessV0ScriptHash& whash = boost::get<WitnessV0ScriptHash>(witness_destination);
                        CScriptID id;
                        CRIPEMD160().Write(whash.begin(), whash.size()).Finalize(id.begin());
                        CScript witnessScript;
                        if (pwallet->GetCScript(id, witnessScript)) {
                            entry.pushKV("witnessScript", HexStr(witnessScript.begin(), witnessScript.end()));
                        }
                    }
                }
            } else if (scriptPubKey.IsPayToWitnessScriptHash()) {
                const WitnessV0ScriptHash& whash = boost::get<WitnessV0ScriptHash>(address);
                CScriptID id;
                CRIPEMD160().Write(whash.begin(), whash.size()).Finalize(id.begin());
                CScript witnessScript;
                if (pwallet->GetCScript(id, witnessScript)) {
                    entry.pushKV("witnessScript", HexStr(witnessScript.begin(), witnessScript.end()));
                }
            }
        }

        entry.pushKV("scriptPubKey", HexStr(scriptPubKey.begin(), scriptPubKey.end()));
        entry.pushKV("amount", ValueFromAmount(out.tx->tx->vout[out.i].nValue));
        entry.pushKV("confirmations", out.nDepth);
        entry.pushKV("spendable", out.fSpendable);
        entry.pushKV("solvable", out.fSolvable);
        if (out.fSolvable) {
            auto descriptor = InferDescriptor(scriptPubKey, *pwallet);
            entry.pushKV("desc", descriptor->ToString());
        }
        entry.pushKV("safe", out.fSafe);
        results.push_back(entry);
    }

    return results;
}

void FundTransaction(CWallet* const pwallet, CMutableTransaction& tx, CAmount& fee_out, int& change_position, UniValue options)
{
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    CCoinControl coinControl;
    change_position = -1;
    bool lockUnspents = false;
    UniValue subtractFeeFromOutputs;
    std::set<int> setSubtractFeeFromOutputs;

    if (!options.isNull()) {
        if (options.type() == UniValue::VBOOL) {
            // backward compatibility bool only fallback
            coinControl.fAllowWatchOnly = options.get_bool();
        } else {
            RPCTypeCheckArgument(options, UniValue::VOBJ);
            RPCTypeCheckObj(options,
                {
                    {"changeAddress", UniValueType(UniValue::VSTR)},
                    {"changePosition", UniValueType(UniValue::VNUM)},
                    {"change_type", UniValueType(UniValue::VSTR)},
                    {"includeWatching", UniValueType(UniValue::VBOOL)},
                    {"lockUnspents", UniValueType(UniValue::VBOOL)},
                    {"feeRate", UniValueType()}, // will be checked below
                    {"subtractFeeFromOutputs", UniValueType(UniValue::VARR)},
                    {"replaceable", UniValueType(UniValue::VBOOL)},
                    {"conf_target", UniValueType(UniValue::VNUM)},
                    {"estimate_mode", UniValueType(UniValue::VSTR)},
                },
                true, true);

            if (options.exists("changeAddress")) {
                CTxDestination dest = DecodeDestination(options["changeAddress"].get_str());

                if (!IsValidDestination(dest)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "changeAddress must be a valid HYDRA address");
                }

                coinControl.destChange = dest;
            }

            if (options.exists("changePosition"))
                change_position = options["changePosition"].get_int();

            if (options.exists("change_type")) {
                if (options.exists("changeAddress")) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both changeAddress and address_type options");
                }
                coinControl.m_change_type = pwallet->m_default_change_type;
                if (!ParseOutputType(options["change_type"].get_str(), *coinControl.m_change_type)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown change type '%s'", options["change_type"].get_str()));
                }
            }

            if (options.exists("includeWatching"))
                coinControl.fAllowWatchOnly = options["includeWatching"].get_bool();

            if (options.exists("lockUnspents"))
                lockUnspents = options["lockUnspents"].get_bool();

            if (options.exists("feeRate")) {
                coinControl.m_feerate = CFeeRate(AmountFromValue(options["feeRate"]));
                coinControl.fOverrideFeeRate = true;
            }

            if (options.exists("subtractFeeFromOutputs"))
                subtractFeeFromOutputs = options["subtractFeeFromOutputs"].get_array();

            if (options.exists("replaceable")) {
                coinControl.m_signal_bip125_rbf = options["replaceable"].get_bool();
            }
            if (options.exists("conf_target")) {
                if (options.exists("feeRate")) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both conf_target and feeRate");
                }
                coinControl.m_confirm_target = ParseConfirmTarget(options["conf_target"]);
            }
            if (options.exists("estimate_mode")) {
                if (options.exists("feeRate")) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both estimate_mode and feeRate");
                }
                if (!FeeModeFromString(options["estimate_mode"].get_str(), coinControl.m_fee_mode)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
                }
            }
        }
    }

    if (tx.vout.size() == 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "TX must have at least one output");

    if (change_position != -1 && (change_position < 0 || (unsigned int)change_position > tx.vout.size()))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "changePosition out of bounds");

    for (unsigned int idx = 0; idx < subtractFeeFromOutputs.size(); idx++) {
        int pos = subtractFeeFromOutputs[idx].get_int();
        if (setSubtractFeeFromOutputs.count(pos))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, duplicated position: %d", pos));
        if (pos < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, negative position: %d", pos));
        if (pos >= int(tx.vout.size()))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, position too large: %d", pos));
        setSubtractFeeFromOutputs.insert(pos);
    }

    std::string strFailReason;

    if (!pwallet->FundTransaction(tx, fee_out, change_position, strFailReason, lockUnspents, setSubtractFeeFromOutputs, coinControl)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strFailReason);
    }
}

static UniValue fundrawtransaction(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    const RPCHelpMan help{
        "fundrawtransaction",
        "\nAdd inputs to a transaction until it has enough in value to meet its out value.\n"
        "This will not modify existing inputs, and will add at most one change output to the outputs.\n"
        "No existing outputs will be modified unless \"subtractFeeFromOutputs\" is specified.\n"
        "Note that inputs which were signed may need to be resigned after completion since in/outputs have been added.\n"
        "The inputs added will not be signed, use signrawtransactionwithkey\n"
        " or signrawtransactionwithwallet for that.\n"
        "Note that all existing inputs must have their previous output transaction be in the wallet.\n"
        "Note that all inputs selected must be of standard form and P2SH scripts must be\n"
        "in the wallet using importaddress or addmultisigaddress (to calculate fees).\n"
        "You can see whether this is the case by checking the \"solvable\" field in the listunspent output.\n"
        "Only pay-to-pubkey, multisig, and P2SH versions thereof are currently supported for watch-only\n",
        {
            {"hexstring", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hex string of the raw transaction"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "for backward compatibility: passing in a true instead of an object will result in {\"includeWatching\":true}",
                {
                    {"changeAddress", RPCArg::Type::STR, /* default */ "pool address", "The HYDRA address to receive the change"},
                    {"changePosition", RPCArg::Type::NUM, /* default */ "random", "The index of the change output"},
                    {"change_type", RPCArg::Type::STR, /* default */ "set by -changetype", "The output type to use. Only valid if changeAddress is not specified. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                    {"includeWatching", RPCArg::Type::BOOL, /* default */ "false", "Also select inputs which are watch only"},
                    {"lockUnspents", RPCArg::Type::BOOL, /* default */ "false", "Lock selected unspent outputs"},
                    {"feeRate", RPCArg::Type::AMOUNT, /* default */ "not set: makes wallet determine the fee", "Set a specific fee rate in " + CURRENCY_UNIT + "/kB"},
                    {
                        "subtractFeeFromOutputs",
                        RPCArg::Type::ARR,
                        /* default */ "empty array",
                        "A json array of integers.\n"
                        "                              The fee will be equally deducted from the amount of each specified output.\n"
                        "                              Those recipients will receive less HYDRA than you enter in their corresponding amount field.\n"
                        "                              If no outputs are specified here, the sender pays the fee.",
                        {
                            {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                        },
                    },
                    {"replaceable", RPCArg::Type::BOOL, /* default */ "fallback to wallet's default", "Marks this transaction as BIP125 replaceable.\n"
                                                                                                      "                              Allows this transaction to be replaced by a transaction with higher fees"},
                    {"conf_target", RPCArg::Type::NUM, /* default */ "fallback to wallet's default", "Confirmation target (in blocks)"},
                    {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
                                                                                "         \"UNSET\"\n"
                                                                                "         \"ECONOMICAL\"\n"
                                                                                "         \"CONSERVATIVE\""},
                },
                "options"},
            {"iswitness", RPCArg::Type::BOOL, /* default */ "depends on heuristic tests", "Whether the transaction hex is a serialized witness transaction.\n"
                                                                                          "If iswitness is not present, heuristic tests will be used in decoding.\n"
                                                                                          "If true, only witness deserialization will be tried.\n"
                                                                                          "If false, only non-witness deserialization will be tried.\n"
                                                                                          "This boolean should reflect whether the transaction has inputs\n"
                                                                                          "(e.g. fully valid, or on-chain transactions), if known by the caller."},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "hex", "The resulting raw transaction (hex-encoded string)"},
                {RPCResult::Type::STR_AMOUNT, "fee", "Fee in " + CURRENCY_UNIT + " the resulting transaction pays"},
                {RPCResult::Type::NUM, "changepos", "The position of the added change output, or -1"},
            }},
        RPCExamples{
            "\nCreate a transaction with no inputs\n" + HelpExampleCli("createrawtransaction", "\"[]\" \"{\\\"myaddress\\\":0.01}\"") +
            "\nAdd sufficient unsigned inputs to meet the output value\n" + HelpExampleCli("fundrawtransaction", "\"rawtransactionhex\"") +
            "\nSign the transaction\n" + HelpExampleCli("signrawtransactionwithwallet", "\"fundedtransactionhex\"") +
            "\nSend the transaction\n" + HelpExampleCli("sendrawtransaction", "\"signedtransactionhex\"")},
    };

    if (request.fHelp || !help.IsValidNumArgs(request.params.size())) {
        throw std::runtime_error(help.ToString());
    }

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValueType(), UniValue::VBOOL});

    // parse hex string from parameter
    CMutableTransaction tx;
    bool try_witness = request.params[2].isNull() ? true : request.params[2].get_bool();
    bool try_no_witness = request.params[2].isNull() ? true : !request.params[2].get_bool();
    if (!DecodeHexTx(tx, request.params[0].get_str(), try_no_witness, try_witness)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    CAmount fee;
    int change_position;
    FundTransaction(pwallet, tx, fee, change_position, request.params[1]);

    UniValue result(UniValue::VOBJ);
    result.pushKV("hex", EncodeHexTx(CTransaction(tx)));
    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("changepos", change_position);

    return result;
}

UniValue signrawtransactionwithwallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
            RPCHelpMan{
                "signrawtransactionwithwallet",
                "\nSign inputs for raw transaction (serialized, hex-encoded).\n"
                "The second optional argument (may be null) is an array of previous transaction outputs that\n"
                "this transaction depends on but may not yet be in the block chain." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"hexstring", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction hex string"},
                    {
                        "prevtxs",
                        RPCArg::Type::ARR,
                        RPCArg::Optional::OMITTED_NAMED_ARG,
                        "A json array of previous dependent transaction outputs",
                        {
                            {
                                "",
                                RPCArg::Type::OBJ,
                                RPCArg::Optional::OMITTED,
                                "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"scriptPubKey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "script key"},
                                    {"redeemScript", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "(required for P2SH) redeem script"},
                                    {"witnessScript", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "(required for P2WSH or P2SH-P2WSH) witness script"},
                                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount spent"},
                                },
                            },
                        },
                    },
                    {"sighashtype", RPCArg::Type::STR, /* default */ "ALL", "The signature hash type. Must be one of\n"
                                                                            "       \"ALL\"\n"
                                                                            "       \"NONE\"\n"
                                                                            "       \"SINGLE\"\n"
                                                                            "       \"ALL|ANYONECANPAY\"\n"
                                                                            "       \"NONE|ANYONECANPAY\"\n"
                                                                            "       \"SINGLE|ANYONECANPAY\""},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hex", "The hex-encoded raw transaction with signature(s)"},
                        {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                        {RPCResult::Type::ARR, "errors", "Script verification errors (if there are any)",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                    {
                                        {RPCResult::Type::STR_HEX, "txid", "The hash of the referenced, previous transaction"},
                                        {RPCResult::Type::NUM, "vout", "The index of the output to spent and used as input"},
                                        {RPCResult::Type::STR_HEX, "scriptSig", "The hex-encoded signature script"},
                                        {RPCResult::Type::NUM, "sequence", "Script sequence number"},
                                        {RPCResult::Type::STR, "error", "Verification or signing error related to the input"},
                                    }},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("signrawtransactionwithwallet", "\"myhex\"") + HelpExampleRpc("signrawtransactionwithwallet", "\"myhex\"")},
            }
                .ToString());

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VARR, UniValue::VSTR}, true);

    CMutableTransaction mtx;
    if (!DecodeHexTx(mtx, request.params[0].get_str(), true)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    // Sign the transaction
    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(pwallet);

    return SignTransaction(pwallet->chain(), mtx, request.params[1], pwallet, false, request.params[2]);
}

UniValue signrawsendertransactionwithwallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            RPCHelpMan{
                "signrawsendertransactionwithwallet",
                "\nSign OP_SENDER outputs for raw transaction (serialized, hex-encoded).\n" +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"hexstring", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction hex string"},
                    {"sighashtype", RPCArg::Type::STR, /* default */ "ALL", "The signature hash type. Must be one of\n"
                                                                            "       \"ALL\"\n"
                                                                            "       \"NONE\"\n"
                                                                            "       \"SINGLE\"\n"
                                                                            "       \"ALL|ANYONECANPAY\"\n"
                                                                            "       \"NONE|ANYONECANPAY\"\n"
                                                                            "       \"SINGLE|ANYONECANPAY\""},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hex", "The hex-encoded raw transaction with signature(s)"},
                        {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                        {RPCResult::Type::ARR, "errors", "Script verification errors (if there are any)",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                    {
                                        {RPCResult::Type::STR_AMOUNT, "amount", "The amount of the output"},
                                        {RPCResult::Type::STR_HEX, "scriptPubKey", "The hex-encoded public key script of the output"},
                                        {RPCResult::Type::STR, "error", "Verification or signing error related to the output"},
                                    }},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("signrawsendertransactionwithwallet", "\"myhex\"") + HelpExampleRpc("signrawsendertransactionwithwallet", "\"myhex\"")},
            }
                .ToString());

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VSTR}, true);

    CMutableTransaction mtx;
    if (!DecodeHexTx(mtx, request.params[0].get_str(), true)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    // Sign the transaction
    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(pwallet);

    return SignTransactionSender(pwallet->chain(), mtx, pwallet, request.params[1]);
}

static UniValue bumpfee(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();


    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2) {
        throw std::runtime_error(
            RPCHelpMan{
                "bumpfee",
                "\nBumps the fee of an opt-in-RBF transaction T, replacing it with a new transaction B.\n"
                "An opt-in RBF transaction with the given txid must be in the wallet.\n"
                "The command will pay the additional fee by decreasing (or perhaps removing) its change output.\n"
                "If the change output is not big enough to cover the increased fee, the command will currently fail\n"
                "instead of adding new inputs to compensate. (A future implementation could improve this.)\n"
                "The command will fail if the wallet or mempool contains a transaction that spends one of T's outputs.\n"
                "By default, the new fee will be calculated automatically using estimatesmartfee.\n"
                "The user can specify a confirmation target for estimatesmartfee.\n"
                "Alternatively, the user can specify totalFee, or use RPC settxfee to set a higher fee rate.\n"
                "At a minimum, the new fee rate must be high enough to pay an additional new relay fee (incrementalfee\n"
                "returned by getnetworkinfo) to enter the node's mempool.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The txid to be bumped"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "",
                        {
                            {"confTarget", RPCArg::Type::NUM, /* default */ "fallback to wallet's default", "Confirmation target (in blocks)"},
                            {"totalFee", RPCArg::Type::NUM, /* default */ "fallback to 'confTarget'", "Total fee (NOT feerate) to pay, in satoshis.\n"
                                                                                                      "                         In rare cases, the actual fee paid might be slightly higher than the specified\n"
                                                                                                      "                         totalFee if the tx change output has to be removed because it is too close to\n"
                                                                                                      "                         the dust threshold."},
                            {"replaceable", RPCArg::Type::BOOL, /* default */ "true", "Whether the new transaction should still be\n"
                                                                                      "                         marked bip-125 replaceable. If true, the sequence numbers in the transaction will\n"
                                                                                      "                         be left unchanged from the original. If false, any input sequence numbers in the\n"
                                                                                      "                         original transaction that were less than 0xfffffffe will be increased to 0xfffffffe\n"
                                                                                      "                         so the new transaction will not be explicitly bip-125 replaceable (though it may\n"
                                                                                      "                         still be replaceable in practice, for example if it has unconfirmed ancestors which\n"
                                                                                      "                         are replaceable)."},
                            {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
                                                                                        "         \"UNSET\"\n"
                                                                                        "         \"ECONOMICAL\"\n"
                                                                                        "         \"CONSERVATIVE\""},
                        },
                        "options"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "", {
                                                      {RPCResult::Type::STR_HEX, "txid", "The id of the new transaction. Only returned when wallet private keys are enabled."},
                                                      {RPCResult::Type::STR_AMOUNT, "origfee", "The fee of the replaced transaction."},
                                                      {RPCResult::Type::STR_AMOUNT, "fee", "The fee of the new transaction."},
                                                      {RPCResult::Type::ARR, "errors", "Errors encountered during processing (may be empty).", {
                                                                                                                                                   {RPCResult::Type::STR, "", ""},
                                                                                                                                               }},
                                                  }},
                RPCExamples{"\nBump the fee, get the new transaction\'s txid\n" + HelpExampleCli("bumpfee", "<txid>")},
            }
                .ToString());
    }

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VOBJ});
    uint256 hash(ParseHashV(request.params[0], "txid"));

    // optional parameters
    CAmount totalFee = 0;
    CCoinControl coin_control;
    coin_control.m_signal_bip125_rbf = true;
    if (!request.params[1].isNull()) {
        UniValue options = request.params[1];
        RPCTypeCheckObj(options,
            {
                {"confTarget", UniValueType(UniValue::VNUM)},
                {"totalFee", UniValueType(UniValue::VNUM)},
                {"replaceable", UniValueType(UniValue::VBOOL)},
                {"estimate_mode", UniValueType(UniValue::VSTR)},
            },
            true, true);

        if (options.exists("confTarget") && options.exists("totalFee")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "confTarget and totalFee options should not both be set. Please provide either a confirmation target for fee estimation or an explicit total fee for the transaction.");
        } else if (options.exists("confTarget")) { // TODO: alias this to conf_target
            coin_control.m_confirm_target = ParseConfirmTarget(options["confTarget"]);
        } else if (options.exists("totalFee")) {
            totalFee = options["totalFee"].get_int64();
            if (totalFee <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid totalFee %s (must be greater than 0)", FormatMoney(totalFee)));
            }
        }

        if (options.exists("replaceable")) {
            coin_control.m_signal_bip125_rbf = options["replaceable"].get_bool();
        }
        if (options.exists("estimate_mode")) {
            if (!FeeModeFromString(options["estimate_mode"].get_str(), coin_control.m_fee_mode)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
            }
        }
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(pwallet);


    std::vector<std::string> errors;
    CAmount old_fee;
    CAmount new_fee;
    CMutableTransaction mtx;
    feebumper::Result res = feebumper::CreateTransaction(pwallet, hash, coin_control, totalFee, errors, old_fee, new_fee, mtx);
    if (res != feebumper::Result::OK) {
        switch (res) {
        case feebumper::Result::INVALID_ADDRESS_OR_KEY:
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, errors[0]);
            break;
        case feebumper::Result::INVALID_REQUEST:
            throw JSONRPCError(RPC_INVALID_REQUEST, errors[0]);
            break;
        case feebumper::Result::INVALID_PARAMETER:
            throw JSONRPCError(RPC_INVALID_PARAMETER, errors[0]);
            break;
        case feebumper::Result::WALLET_ERROR:
            throw JSONRPCError(RPC_WALLET_ERROR, errors[0]);
            break;
        default:
            throw JSONRPCError(RPC_MISC_ERROR, errors[0]);
            break;
        }
    }

    // sign bumped transaction
    if (!feebumper::SignTransaction(pwallet, mtx)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Can't sign transaction.");
    }
    // commit the bumped transaction
    uint256 txid;
    if (feebumper::CommitTransaction(pwallet, hash, std::move(mtx), errors, txid) != feebumper::Result::OK) {
        throw JSONRPCError(RPC_WALLET_ERROR, errors[0]);
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", txid.GetHex());
    result.pushKV("origfee", ValueFromAmount(old_fee));
    result.pushKV("fee", ValueFromAmount(new_fee));
    UniValue result_errors(UniValue::VARR);
    for (const std::string& error : errors) {
        result_errors.push_back(error);
    }
    result.pushKV("errors", result_errors);

    return result;
}

UniValue generate(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();


    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2) {
        throw std::runtime_error(
            RPCHelpMan{
                "generate",
                "\nMine up to nblocks blocks immediately (before the RPC call returns) to an address in the wallet.\n",
                {
                    {"nblocks", RPCArg::Type::NUM, RPCArg::Optional::NO, "How many blocks are generated immediately."},
                    {"maxtries", RPCArg::Type::NUM, /* default */ "1000000", "How many iterations to try."},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "hashes of blocks generated",
                    {
                        {RPCResult::Type::STR_HEX, "", "blockhash"},
                    }},
                RPCExamples{
                    "\nGenerate 11 blocks\n" + HelpExampleCli("generate", "11")},
            }
                .ToString());
    }

    if (!IsDeprecatedRPCEnabled("generate")) {
        throw JSONRPCError(RPC_METHOD_DEPRECATED, "The wallet generate rpc method is deprecated and will be fully removed in v0.19. "
                                                  "To use generate in v0.18, restart hydrad with -deprecatedrpc=generate.\n"
                                                  "Clients should transition to using the node rpc method generatetoaddress\n");
    }

    int num_generate = request.params[0].get_int();
    uint64_t max_tries = 1000000;
    if (!request.params[1].isNull()) {
        max_tries = request.params[1].get_int();
    }

    std::shared_ptr<CReserveScript> coinbase_script;
    pwallet->GetScriptForMining(coinbase_script);

    // If the keypool is exhausted, no script is returned at all.  Catch this.
    if (!coinbase_script) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    }

    // throw an error if no script was provided
    if (coinbase_script->reserveScript.empty()) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No coinbase script available");
    }

    return generateBlocks(coinbase_script, num_generate, max_tries, true);
}

UniValue rescanblockchain(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 2) {
        throw std::runtime_error(
            RPCHelpMan{
                "rescanblockchain",
                "\nRescan the local blockchain for wallet related transactions.\n",
                {
                    {"start_height", RPCArg::Type::NUM, /* default */ "0", "block height where the rescan should start"},
                    {"stop_height", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "the last block height that should be scanned. If none is provided it will rescan up to the tip at return time of this call."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "start_height", "The block height where the rescan started (the requested height or 0)"},
                        {RPCResult::Type::NUM, "stop_height", "The height of the last rescanned block. May be null in rare cases if there was a reorg and the call didn't scan any blocks because they were already scanned in the background."},
                    }},
                RPCExamples{
                    HelpExampleCli("rescanblockchain", "100000 120000") + HelpExampleRpc("rescanblockchain", "100000, 120000")},
            }
                .ToString());
    }

    WalletRescanReserver reserver(pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    int start_height = 0;
    uint256 start_block, stop_block;
    {
        auto locked_chain = pwallet->chain().lock();
        Optional<int> tip_height = locked_chain->getHeight();

        if (!request.params[0].isNull()) {
            start_height = request.params[0].get_int();
            if (start_height < 0 || !tip_height || start_height > *tip_height) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid start_height");
            }
        }

        Optional<int> stop_height;
        if (!request.params[1].isNull()) {
            stop_height = request.params[1].get_int();
            if (*stop_height < 0 || !tip_height || *stop_height > *tip_height) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid stop_height");
            } else if (*stop_height < start_height) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "stop_height must be greater than start_height");
            }
        }

        // We can't rescan beyond non-pruned blocks, stop and throw an error
        if (locked_chain->findPruned(start_height, stop_height)) {
            throw JSONRPCError(RPC_MISC_ERROR, "Can't rescan beyond pruned data. Use RPC call getblockchaininfo to determine your pruned height.");
        }

        if (tip_height) {
            start_block = locked_chain->getBlockHash(start_height);
            // If called with a stop_height, set the stop_height here to
            // trigger a rescan to that height.
            // If called without a stop height, leave stop_height as null here
            // so rescan continues to the tip (even if the tip advances during
            // rescan).
            if (stop_height) {
                stop_block = locked_chain->getBlockHash(*stop_height);
            }
        }
    }

    CWallet::ScanResult result =
        pwallet->ScanForWalletTransactions(start_block, stop_block, reserver, true /* fUpdate */);
    switch (result.status) {
    case CWallet::ScanResult::SUCCESS:
        break;
    case CWallet::ScanResult::FAILURE:
        throw JSONRPCError(RPC_MISC_ERROR, "Rescan failed. Potentially corrupted data files.");
    case CWallet::ScanResult::USER_ABORT:
        throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted.");
        // no default case, so the compiler can warn about missing cases
    }
    UniValue response(UniValue::VOBJ);
    response.pushKV("start_height", start_height);
    response.pushKV("stop_height", result.last_scanned_height ? *result.last_scanned_height : UniValue());
    return response;
}

class DescribeWalletAddressVisitor : public boost::static_visitor<UniValue>
{
public:
    CWallet* const pwallet;

    void ProcessSubScript(const CScript& subscript, UniValue& obj) const
    {
        // Always present: script type and redeemscript
        std::vector<std::vector<unsigned char>> solutions_data;
        txnouttype which_type = Solver(subscript, solutions_data);
        obj.pushKV("script", GetTxnOutputType(which_type));
        obj.pushKV("hex", HexStr(subscript.begin(), subscript.end()));

        CTxDestination embedded;
        if (ExtractDestination(subscript, embedded)) {
            // Only when the script corresponds to an address.
            UniValue subobj(UniValue::VOBJ);
            UniValue detail = DescribeAddress(embedded);
            subobj.pushKVs(detail);
            UniValue wallet_detail = boost::apply_visitor(*this, embedded);
            subobj.pushKVs(wallet_detail);
            subobj.pushKV("address", EncodeDestination(embedded));
            subobj.pushKV("scriptPubKey", HexStr(subscript.begin(), subscript.end()));
            // Always report the pubkey at the top level, so that `getnewaddress()['pubkey']` always works.
            if (subobj.exists("pubkey")) obj.pushKV("pubkey", subobj["pubkey"]);
            obj.pushKV("embedded", std::move(subobj));
        } else if (which_type == TX_MULTISIG) {
            // Also report some information on multisig scripts (which do not have a corresponding address).
            // TODO: abstract out the common functionality between this logic and ExtractDestinations.
            obj.pushKV("sigsrequired", solutions_data[0][0]);
            UniValue pubkeys(UniValue::VARR);
            for (size_t i = 1; i < solutions_data.size() - 1; ++i) {
                CPubKey key(solutions_data[i].begin(), solutions_data[i].end());
                pubkeys.push_back(HexStr(key.begin(), key.end()));
            }
            obj.pushKV("pubkeys", std::move(pubkeys));
        }
    }

    explicit DescribeWalletAddressVisitor(CWallet* _pwallet) : pwallet(_pwallet) {}

    UniValue operator()(const CNoDestination& dest) const { return UniValue(UniValue::VOBJ); }

    UniValue operator()(const CKeyID& keyID) const
    {
        UniValue obj(UniValue::VOBJ);
        CPubKey vchPubKey;
        if (pwallet && pwallet->GetPubKey(keyID, vchPubKey)) {
            obj.pushKV("pubkey", HexStr(vchPubKey));
            obj.pushKV("iscompressed", vchPubKey.IsCompressed());
        }
        return obj;
    }

    UniValue operator()(const CScriptID& scriptID) const
    {
        UniValue obj(UniValue::VOBJ);
        CScript subscript;
        if (pwallet && pwallet->GetCScript(scriptID, subscript)) {
            ProcessSubScript(subscript, obj);
        }
        return obj;
    }

    UniValue operator()(const WitnessV0KeyHash& id) const
    {
        UniValue obj(UniValue::VOBJ);
        CPubKey pubkey;
        if (pwallet && pwallet->GetPubKey(CKeyID(id), pubkey)) {
            obj.pushKV("pubkey", HexStr(pubkey));
        }
        return obj;
    }

    UniValue operator()(const WitnessV0ScriptHash& id) const
    {
        UniValue obj(UniValue::VOBJ);
        CScript subscript;
        CRIPEMD160 hasher;
        uint160 hash;
        hasher.Write(id.begin(), 32).Finalize(hash.begin());
        if (pwallet && pwallet->GetCScript(CScriptID(hash), subscript)) {
            ProcessSubScript(subscript, obj);
        }
        return obj;
    }

    UniValue operator()(const WitnessUnknown& id) const { return UniValue(UniValue::VOBJ); }
};

static UniValue DescribeWalletAddress(CWallet* pwallet, const CTxDestination& dest)
{
    UniValue ret(UniValue::VOBJ);
    UniValue detail = DescribeAddress(dest);
    ret.pushKVs(detail);
    ret.pushKVs(boost::apply_visitor(DescribeWalletAddressVisitor(pwallet), dest));
    return ret;
}

/** Convert CAddressBookData to JSON record.  */
static UniValue AddressBookDataToJSON(const CAddressBookData& data, const bool verbose)
{
    UniValue ret(UniValue::VOBJ);
    if (verbose) {
        ret.pushKV("name", data.name);
    }
    ret.pushKV("purpose", data.purpose);
    return ret;
}

UniValue getaddressinfo(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            RPCHelpMan{"getaddressinfo",
                "\nReturn information about the given HYDRA address. Some information requires the address\n"
                "to be in the wallet.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The HYDRA address to get the information of."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "address", "The bitcoin address validated."},
                        {RPCResult::Type::STR_HEX, "scriptPubKey", "The hex-encoded scriptPubKey generated by the address."},
                        {RPCResult::Type::BOOL, "ismine", "If the address is yours."},
                        {RPCResult::Type::BOOL, "iswatchonly", "If the address is watchonly."},
                        {RPCResult::Type::BOOL, "solvable", "If we know how to spend coins sent to this address, ignoring the possible lack of private keys."},
                        {RPCResult::Type::STR, "desc", /* optional */ true, "A descriptor for spending coins sent to this address (only when solvable)."},
                        {RPCResult::Type::BOOL, "isscript", "If the key is a script."},
                        {RPCResult::Type::BOOL, "ischange", "If the address was used for change output."},
                        {RPCResult::Type::BOOL, "iswitness", "If the address is a witness address."},
                        {RPCResult::Type::NUM, "witness_version", /* optional */ true, "The version number of the witness program."},
                        {RPCResult::Type::STR_HEX, "witness_program", /* optional */ true, "The hex value of the witness program."},
                        {RPCResult::Type::STR, "script", /* optional */ true, "The output script type. Only if isscript is true and the redeemscript is known. Possible\n"
                                                                              "types: nonstandard, pubkey, pubkeyhash, scripthash, multisig, nulldata, witness_v0_keyhash, witness_v0_scripthash, witness_unknown."},
                        {RPCResult::Type::STR_HEX, "hex", /* optional */ true, "The redeemscript for the p2sh address."},
                        {RPCResult::Type::ARR, "pubkeys", /* optional */ true, "Array of pubkeys associated with the known redeemscript (only if script is multisig).",
                            {
                                {RPCResult::Type::STR, "pubkey", ""},
                            }},
                        {RPCResult::Type::NUM, "sigsrequired", /* optional */ true, "The number of signatures required to spend multisig output (only if script is multisig)."},
                        {RPCResult::Type::STR_HEX, "pubkey", /* optional */ true, "The hex value of the raw public key for single-key addresses (possibly embedded in P2SH or P2WSH)."},
                        {RPCResult::Type::OBJ, "embedded", /* optional */ true, "Information about the address embedded in P2SH or P2WSH, if relevant and known.",
                            {
                                {RPCResult::Type::ELISION, "", "Includes all\n"
                                                               "getaddressinfo output fields for the embedded address, excluding metadata (timestamp, hdkeypath,\n"
                                                               "hdseedid) and relation to the wallet (ismine, iswatchonly)."},
                            }},
                        {RPCResult::Type::BOOL, "iscompressed", /* optional */ true, "If the pubkey is compressed."},
                        {RPCResult::Type::STR, "label", "DEPRECATED. The label associated with the address. Defaults to \"\". Replaced by the labels array below."},
                        {RPCResult::Type::NUM_TIME, "timestamp", /* optional */ true, "The creation time of the key, if available, expressed in " + UNIX_EPOCH_TIME + "."},
                        {RPCResult::Type::STR, "hdkeypath", /* optional */ true, "The HD keypath, if the key is HD and available."},
                        {RPCResult::Type::STR_HEX, "hdseedid", /* optional */ true, "The Hash160 of the HD seed."},
                        {RPCResult::Type::STR_HEX, "hdmasterfingerprint", /* optional */ true, "The fingerprint of the master key."},
                        {RPCResult::Type::ARR, "labels", "Array of labels associated with the address. Currently limited to one label but returned\n"
                                                         "as an array to keep the API stable if multiple labels are enabled in the future.",
                            {
                                {RPCResult::Type::STR, "label name", "The label name. Defaults to \"\"."},
                                {RPCResult::Type::OBJ, "", "label data, DEPRECATED, will be removed in 0.21. To re-enable, launch bitcoind with `-deprecatedrpc=labelspurpose`",
                                    {
                                        {RPCResult::Type::STR, "name", "The label name. Defaults to \"\"."},
                                        {RPCResult::Type::STR, "purpose", "The purpose of the associated address (send or receive)."},
                                    }},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("getaddressinfo", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\"") + HelpExampleRpc("getaddressinfo", "\"HBvKE1Vk4gDgu5j7TZUX9P3QMAhVErMYoC\"")}}
                .ToString());
    }

    LOCK(pwallet->cs_wallet);

    UniValue ret(UniValue::VOBJ);
    CTxDestination dest = DecodeDestination(request.params[0].get_str());

    // Make sure the destination is valid
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    std::string currentAddress = EncodeDestination(dest);
    ret.pushKV("address", currentAddress);

    CScript scriptPubKey = GetScriptForDestination(dest);
    ret.pushKV("scriptPubKey", HexStr(scriptPubKey.begin(), scriptPubKey.end()));

    isminetype mine = IsMine(*pwallet, dest);
    ret.pushKV("ismine", bool(mine & ISMINE_SPENDABLE));
    bool solvable = IsSolvable(*pwallet, scriptPubKey);
    ret.pushKV("solvable", solvable);
    if (solvable) {
        ret.pushKV("desc", InferDescriptor(scriptPubKey, *pwallet)->ToString());
    }
    ret.pushKV("iswatchonly", bool(mine & ISMINE_WATCH_ONLY));
    UniValue detail = DescribeWalletAddress(pwallet, dest);
    ret.pushKVs(detail);
    if (pwallet->mapAddressBook.count(dest)) {
        ret.pushKV("label", pwallet->mapAddressBook[dest].name);
    }
    ret.pushKV("ischange", pwallet->IsChange(scriptPubKey));
    const CKeyMetadata* meta = nullptr;
    CKeyID key_id = GetKeyForDestination(*pwallet, dest);
    if (!key_id.IsNull()) {
        auto it = pwallet->mapKeyMetadata.find(key_id);
        if (it != pwallet->mapKeyMetadata.end()) {
            meta = &it->second;
        }
    }
    if (!meta) {
        auto it = pwallet->m_script_metadata.find(CScriptID(scriptPubKey));
        if (it != pwallet->m_script_metadata.end()) {
            meta = &it->second;
        }
    }
    if (meta) {
        ret.pushKV("timestamp", meta->nCreateTime);
        if (meta->has_key_origin) {
            ret.pushKV("hdkeypath", WriteHDKeypath(meta->key_origin.path));
            ret.pushKV("hdseedid", meta->hd_seed_id.GetHex());
            ret.pushKV("hdmasterfingerprint", HexStr(meta->key_origin.fingerprint, meta->key_origin.fingerprint + 4));
        }
    }

    // Currently only one label can be associated with an address, return an array
    // so the API remains stable if we allow multiple labels to be associated with
    // an address.
    UniValue labels(UniValue::VARR);
    std::map<CTxDestination, CAddressBookData>::iterator mi = pwallet->mapAddressBook.find(dest);
    if (mi != pwallet->mapAddressBook.end()) {
        labels.push_back(AddressBookDataToJSON(mi->second, true));
    }
    ret.pushKV("labels", std::move(labels));

    return ret;
}

static UniValue getaddressesbylabel(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            RPCHelpMan{
                "getaddressesbylabel",
                "\nReturns the list of addresses assigned the specified label.\n",
                {
                    {"label", RPCArg::Type::STR, RPCArg::Optional::NO, "The label."},
                },
                RPCResult{
                    RPCResult::Type::OBJ_DYN, "", "json object with addresses as keys",
                    {
                        {RPCResult::Type::OBJ, "address", "json object with information about address",
                            {
                                {RPCResult::Type::STR, "purpose", "Purpose of address (\"send\" for sending address, \"receive\" for receiving address)"},
                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("getaddressesbylabel", "\"tabby\"") + HelpExampleRpc("getaddressesbylabel", "\"tabby\"")},
            }
                .ToString());

    LOCK(pwallet->cs_wallet);

    std::string label = LabelFromValue(request.params[0]);

    // Find all addresses that have the given label
    UniValue ret(UniValue::VOBJ);
    for (const std::pair<const CTxDestination, CAddressBookData>& item : pwallet->mapAddressBook) {
        if (item.second.name == label) {
            ret.pushKV(EncodeDestination(item.first), AddressBookDataToJSON(item.second, false));
        }
    }

    if (ret.empty()) {
        throw JSONRPCError(RPC_WALLET_INVALID_LABEL_NAME, std::string("No addresses with label " + label));
    }

    return ret;
}

static UniValue listlabels(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            RPCHelpMan{
                "listlabels",
                "\nReturns the list of all labels, or labels that are assigned to addresses with a specific purpose.\n",
                {
                    {"purpose", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "Address purpose to list labels for ('send','receive'). An empty string is the same as not providing this argument."},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::STR, "label", "Label name"},
                    }},
                RPCExamples{
                    "\nList all labels\n" + HelpExampleCli("listlabels", "") +
                    "\nList labels that have receiving addresses\n" + HelpExampleCli("listlabels", "receive") +
                    "\nList labels that have sending addresses\n" + HelpExampleCli("listlabels", "send") +
                    "\nAs a JSON-RPC call\n" + HelpExampleRpc("listlabels", "receive")},
            }
                .ToString());

    LOCK(pwallet->cs_wallet);

    std::string purpose;
    if (!request.params[0].isNull()) {
        purpose = request.params[0].get_str();
    }

    // Add to a set to sort by label name, then insert into Univalue array
    std::set<std::string> label_set;
    for (const std::pair<const CTxDestination, CAddressBookData>& entry : pwallet->mapAddressBook) {
        if (purpose.empty() || entry.second.purpose == purpose) {
            label_set.insert(entry.second.name);
        }
    }

    UniValue ret(UniValue::VARR);
    for (const std::string& name : label_set) {
        ret.push_back(name);
    }

    return ret;
}

UniValue sethdseed(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 2) {
        throw std::runtime_error(
            RPCHelpMan{
                "sethdseed",
                "\nSet or generate a new HD wallet seed. Non-HD wallets will not be upgraded to being a HD wallet. Wallets that are already\n"
                "HD will have a new HD seed set so that new keys added to the keypool will be derived from this new seed.\n"
                "\nNote that you will need to MAKE A NEW BACKUP of your wallet after setting the HD wallet seed." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"newkeypool", RPCArg::Type::BOOL, /* default */ "true", "Whether to flush old unused addresses, including change addresses, from the keypool and regenerate it.\n"
                                                                             "                             If true, the next address from getnewaddress and change address from getrawchangeaddress will be from this new seed.\n"
                                                                             "                             If false, addresses (including change addresses if the wallet already had HD Chain Split enabled) from the existing\n"
                                                                             "                             keypool will be used until it has been depleted."},
                    {"seed", RPCArg::Type::STR, /* default */ "random seed", "The WIF private key to use as the new HD seed.\n"
                                                                             "                             The seed value can be retrieved using the dumpwallet command. It is the private key marked hdseed=1"},
                },
                RPCResults{},
                RPCExamples{
                    HelpExampleCli("sethdseed", "") + HelpExampleCli("sethdseed", "false") + HelpExampleCli("sethdseed", "true \"wifkey\"") + HelpExampleRpc("sethdseed", "true, \"wifkey\"")},
            }
                .ToString());
    }

    if (IsInitialBlockDownload()) {
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Cannot set a new HD seed while still in Initial Block Download");
    }

    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Cannot set a HD seed to a wallet with private keys disabled");
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    // Do not do anything to non-HD wallets
    if (!pwallet->CanSupportFeature(FEATURE_HD)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Cannot set a HD seed on a non-HD wallet. Start with -upgradewallet in order to upgrade a non-HD wallet to HD");
    }

    EnsureWalletIsUnlocked(pwallet);

    bool flush_key_pool = true;
    if (!request.params[0].isNull()) {
        flush_key_pool = request.params[0].get_bool();
    }

    CPubKey master_pub_key;
    if (request.params[1].isNull()) {
        master_pub_key = pwallet->GenerateNewSeed();
    } else {
        CKey key = DecodeSecret(request.params[1].get_str());
        if (!key.IsValid()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key");
        }

        if (HaveKey(*pwallet, key)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Already have this key (either as an HD seed or as a loose private key)");
        }

        master_pub_key = pwallet->DeriveNewSeed(key);
    }

    pwallet->SetHDSeed(master_pub_key);
    if (flush_key_pool) pwallet->NewKeyPool();

    return NullUniValue;
}

void AddKeypathToMap(const CWallet* pwallet, const CKeyID& keyID, std::map<CPubKey, KeyOriginInfo>& hd_keypaths)
{
    CPubKey vchPubKey;
    if (!pwallet->GetPubKey(keyID, vchPubKey)) {
        return;
    }
    KeyOriginInfo info;
    if (!pwallet->GetKeyOrigin(keyID, info)) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Internal keypath is broken");
    }
    hd_keypaths.emplace(vchPubKey, std::move(info));
}

UniValue walletprocesspsbt(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 4)
        throw std::runtime_error(
            RPCHelpMan{
                "walletprocesspsbt",
                "\nUpdate a PSBT with input information from our wallet and then sign inputs\n"
                "that we can sign for." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"psbt", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction base64 string"},
                    {"sign", RPCArg::Type::BOOL, /* default */ "true", "Also sign the transaction when updating"},
                    {"sighashtype", RPCArg::Type::STR, /* default */ "ALL", "The signature hash type to sign with if not specified by the PSBT. Must be one of\n"
                                                                            "       \"ALL\"\n"
                                                                            "       \"NONE\"\n"
                                                                            "       \"SINGLE\"\n"
                                                                            "       \"ALL|ANYONECANPAY\"\n"
                                                                            "       \"NONE|ANYONECANPAY\"\n"
                                                                            "       \"SINGLE|ANYONECANPAY\""},
                    {"bip32derivs", RPCArg::Type::BOOL, /* default */ "false", "If true, includes the BIP 32 derivation paths for public keys if we know them"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "psbt", "The base64-encoded partially signed transaction"},
                        {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                    }},
                RPCExamples{
                    HelpExampleCli("walletprocesspsbt", "\"psbt\"")},
            }
                .ToString());

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VBOOL, UniValue::VSTR});

    // Unserialize the transaction
    PartiallySignedTransaction psbtx;
    std::string error;
    if (!DecodeBase64PSBT(psbtx, request.params[0].get_str(), error)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("TX decode failed %s", error));
    }

    // Get the sighash type
    int nHashType = ParseSighashString(request.params[2]);

    // Fill transaction with our data and also sign
    bool sign = request.params[1].isNull() ? true : request.params[1].get_bool();
    bool bip32derivs = request.params[3].isNull() ? false : request.params[3].get_bool();
    bool complete = true;
    const TransactionError err = FillPSBT(pwallet, psbtx, complete, nHashType, sign, bip32derivs);
    if (err != TransactionError::OK) {
        throw JSONRPCTransactionError(err);
    }

    UniValue result(UniValue::VOBJ);
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << psbtx;
    result.pushKV("psbt", EncodeBase64(ssTx.str()));
    result.pushKV("complete", complete);

    return result;
}

UniValue walletcreatefundedpsbt(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 5)
        throw std::runtime_error(
            RPCHelpMan{
                "walletcreatefundedpsbt",
                "\nCreates and funds a transaction in the Partially Signed Transaction format. Inputs will be added if supplied inputs are not enough\n"
                "Implements the Creator and Updater roles.\n",
                {
                    {
                        "inputs",
                        RPCArg::Type::ARR,
                        RPCArg::Optional::NO,
                        "A json array of json objects",
                        {
                            {
                                "",
                                RPCArg::Type::OBJ,
                                RPCArg::Optional::OMITTED,
                                "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"sequence", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sequence number"},
                                },
                            },
                        },
                    },
                    {
                        "outputs",
                        RPCArg::Type::ARR,
                        RPCArg::Optional::NO,
                        "a json array with outputs (key-value pairs), where none of the keys are duplicated.\n"
                        "That is, each address can only appear once and there can only be one 'data' object.\n"
                        "For compatibility reasons, a dictionary, which holds the key-value pairs directly, is also\n"
                        "                             accepted as second parameter.",
                        {
                            {
                                "",
                                RPCArg::Type::OBJ,
                                RPCArg::Optional::OMITTED,
                                "",
                                {
                                    {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A key-value pair. The key (string) is the HYDRA address, the value (float or string) is the amount in " + CURRENCY_UNIT + ""},
                                },
                            },
                            {
                                "",
                                RPCArg::Type::OBJ,
                                RPCArg::Optional::OMITTED,
                                "",
                                {
                                    {"data", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A key-value pair. The key must be \"data\", the value is hex-encoded data"},
                                },
                            },
                        },
                    },
                    {"locktime", RPCArg::Type::NUM, /* default */ "0", "Raw locktime. Non-0 value also locktime-activates inputs"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "",
                        {
                            {"changeAddress", RPCArg::Type::STR_HEX, /* default */ "pool address", "The HYDRA address to receive the change"},
                            {"changePosition", RPCArg::Type::NUM, /* default */ "random", "The index of the change output"},
                            {"change_type", RPCArg::Type::STR, /* default */ "set by -changetype", "The output type to use. Only valid if changeAddress is not specified. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                            {"includeWatching", RPCArg::Type::BOOL, /* default */ "false", "Also select inputs which are watch only"},
                            {"lockUnspents", RPCArg::Type::BOOL, /* default */ "false", "Lock selected unspent outputs"},
                            {"feeRate", RPCArg::Type::AMOUNT, /* default */ "not set: makes wallet determine the fee", "Set a specific fee rate in " + CURRENCY_UNIT + "/kB"},
                            {
                                "subtractFeeFromOutputs",
                                RPCArg::Type::ARR,
                                /* default */ "empty array",
                                "A json array of integers.\n"
                                "                              The fee will be equally deducted from the amount of each specified output.\n"
                                "                              Those recipients will receive less HYDRA than you enter in their corresponding amount field.\n"
                                "                              If no outputs are specified here, the sender pays the fee.",
                                {
                                    {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                                },
                            },
                            {"replaceable", RPCArg::Type::BOOL, /* default */ "false", "Marks this transaction as BIP125 replaceable.\n"
                                                                                       "                              Allows this transaction to be replaced by a transaction with higher fees"},
                            {"conf_target", RPCArg::Type::NUM, /* default */ "Fallback to wallet's confirmation target", "Confirmation target (in blocks)"},
                            {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
                                                                                        "         \"UNSET\"\n"
                                                                                        "         \"ECONOMICAL\"\n"
                                                                                        "         \"CONSERVATIVE\""},
                        },
                        "options"},
                    {"bip32derivs", RPCArg::Type::BOOL, /* default */ "false", "If true, includes the BIP 32 derivation paths for public keys if we know them"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "psbt", "The resulting raw transaction (base64-encoded string)"},
                        {RPCResult::Type::STR_AMOUNT, "fee", "Fee in " + CURRENCY_UNIT + " the resulting transaction pays"},
                        {RPCResult::Type::NUM, "changepos", "The position of the added change output, or -1"},
                    }},
                RPCExamples{
                    "\nCreate a transaction with no inputs\n" + HelpExampleCli("walletcreatefundedpsbt", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"[{\\\"data\\\":\\\"00010203\\\"}]\"")},
            }
                .ToString());

    RPCTypeCheck(request.params, {UniValue::VARR,
                                     UniValueType(), // ARR or OBJ, checked later
                                     UniValue::VNUM, UniValue::VOBJ, UniValue::VBOOL},
        true);

    CAmount fee;
    int change_position;
    CMutableTransaction rawTx = ConstructTransaction(request.params[0], request.params[1], request.params[2], request.params[3]["replaceable"]);
    FundTransaction(pwallet, rawTx, fee, change_position, request.params[3]);

    // Make a blank psbt
    PartiallySignedTransaction psbtx(rawTx);

    // Fill transaction with out data but don't sign
    bool bip32derivs = request.params[4].isNull() ? false : request.params[4].get_bool();
    bool complete = true;
    const TransactionError err = FillPSBT(pwallet, psbtx, complete, 1, false, bip32derivs);
    if (err != TransactionError::OK) {
        throw JSONRPCTransactionError(err);
    }

    // Serialize the PSBT
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << psbtx;

    UniValue result(UniValue::VOBJ);
    result.pushKV("psbt", EncodeBase64(ssTx.str()));
    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("changepos", change_position);
    return result;
}

static UniValue qrc20approve(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    QtumDGP qtumDGP(globalState.get(), fGettingValuesDGP);
    uint64_t blockGasLimit = qtumDGP.getBlockGasLimit(chainActive.Height());
    uint64_t minGasPrice = CAmount(qtumDGP.getMinGasPrice(chainActive.Height()));
    PriceOracle oracle;
    uint64_t oracleGasPrice;
    oracle.getPrice(oracleGasPrice);
    CAmount defaultGasPrice = (minGasPrice > DEFAULT_GAS_PRICE) ? minGasPrice : oracleGasPrice;
    Dgp dgp;
    CAmount gasPriceBuffer;
    dgp.calculateGasPriceBuffer(defaultGasPrice, gasPriceBuffer);
    CAmount nGasPrice = gasPriceBuffer + defaultGasPrice;
    uint64_t nGasLimit = DEFAULT_GAS_LIMIT_OP_SEND;
    bool fCheckOutputs = true;

    if (request.fHelp || request.params.size() < 4 || request.params.size() > 6)
        throw std::runtime_error(
            RPCHelpMan{
                "hrc20approve",
                "\nOwner approves an address to spend some amount of tokens.\n",
                {
                    {"contractaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address."},
                    {"owneraddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The tokens owner hydra address."},
                    {"spenderaddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The token spender hydra address."},
                    {"amount", RPCArg::Type::STR, RPCArg::Optional::NO, "The amount of tokens. eg 0.1"},
                    {"gasLimit", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The gas limit, default: " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + ", max: " + i64tostr(blockGasLimit)},
                    {"checkOutputs", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Check outputs before send, default: true"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                    }},
                RPCExamples{
                    HelpExampleCli("hrc20approve", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1") + HelpExampleCli("hrc20approve", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + " true") + HelpExampleRpc("hrc20approve", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1") + HelpExampleRpc("hrc20approve", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + " true")},
            }
                .ToString());

    // Get mandatory parameters
    std::string contract = request.params[0].get_str();
    std::string owner = request.params[1].get_str();
    std::string spender = request.params[2].get_str();
    std::string tokenAmount = request.params[3].get_str();

    // Get gas limit
    if (request.params.size() > 4) {
        nGasLimit = request.params[4].get_int64();
    }

    // Get check outputs flag
    if (request.params.size() > 5) {
        fCheckOutputs = request.params[5].get_bool();
    }

    // Set token parameters
    SendToken token(*locked_chain, pwallet);
    token.setAddress(contract);
    token.setSender(owner);
    token.setGasLimit(i64tostr(nGasLimit));

    // Get decimals
    uint32_t decimals;
    if (!token.decimals(decimals))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get decimals");

    // Get token amount to approve
    dev::s256 nTokenAmount;
    if (!ParseToken(decimals, tokenAmount, nTokenAmount))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get token amount");

    // Check approve offline
    std::string value = nTokenAmount.str();
    bool success = false;
    if (fCheckOutputs) {
        token.setCheckGasForCall(true);
        if (!token.approve(spender, value, success) || !success)
            throw JSONRPCError(RPC_MISC_ERROR, "Fail offline check for approve token amount for spending");
    }

    // Approve value to spend
    if (!token.approve(spender, value, success, true))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to approve token amount for spending");

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", token.getTxId());
    return result;
}

static UniValue qrc20transfer(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    QtumDGP qtumDGP(globalState.get(), fGettingValuesDGP);
    uint64_t blockGasLimit = qtumDGP.getBlockGasLimit(chainActive.Height());
    uint64_t minGasPrice = CAmount(qtumDGP.getMinGasPrice(chainActive.Height()));
    PriceOracle oracle;
    uint64_t oracleGasPrice;
    oracle.getPrice(oracleGasPrice);
    CAmount defaultGasPrice = (minGasPrice > DEFAULT_GAS_PRICE) ? minGasPrice : oracleGasPrice;
    Dgp dgp;
    CAmount gasPriceBuffer;
    dgp.calculateGasPriceBuffer(defaultGasPrice, gasPriceBuffer);
    CAmount nGasPrice = gasPriceBuffer + defaultGasPrice;
    uint64_t nGasLimit = DEFAULT_GAS_LIMIT_OP_SEND;
    bool fCheckOutputs = true;

    if (request.fHelp || request.params.size() < 3 || request.params.size() > 6)
        throw std::runtime_error(
            RPCHelpMan{
                "hrc20transfer",
                "\nSend token amount to a given address.\n",
                {
                    {"contractaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address."},
                    {"owneraddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The tokens owner hydra address."},
                    {"addressto", RPCArg::Type::STR, RPCArg::Optional::NO, "The hydra address to send funds to."},
                    {"amount", RPCArg::Type::STR, RPCArg::Optional::NO, "The amount of tokens to send. eg 0.1"},
                    {"gasLimit", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The gas limit, default: " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + ", max: " + i64tostr(blockGasLimit)},
                    {"checkOutputs", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Check outputs before send, default: true"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                    }},
                RPCExamples{
                    HelpExampleCli("hrc20transfer", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1") + HelpExampleCli("hrc20transfer", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + " true") + HelpExampleRpc("hrc20transfer", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1") + HelpExampleRpc("hrc20transfer", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + " true")},
            }
                .ToString());

    // Get mandatory parameters
    std::string contract = request.params[0].get_str();
    std::string owner = request.params[1].get_str();
    std::string address = request.params[2].get_str();
    std::string tokenAmount = request.params[3].get_str();

    // Get gas limit
    if (request.params.size() > 4) {
        nGasLimit = request.params[4].get_int64();
    }

    // Get check outputs flag
    if (request.params.size() > 5) {
        fCheckOutputs = request.params[5].get_bool();
    }

    // Set token parameters
    SendToken token(*locked_chain, pwallet);
    token.setAddress(contract);
    token.setSender(owner);
    token.setGasLimit(i64tostr(nGasLimit));

    // Get decimals
    uint32_t decimals;
    if (!token.decimals(decimals))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get decimals");

    // Get token amount
    dev::s256 nTokenAmount;
    if (!ParseToken(decimals, tokenAmount, nTokenAmount))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get token amount");

    // Get token owner balance
    std::string strBalance;
    if (!token.balanceOf(strBalance))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get balance");

    // Check if balance is enough to cover it
    dev::s256 balance(strBalance);
    if (balance < nTokenAmount)
        throw JSONRPCError(RPC_MISC_ERROR, "Not enough token balance");

    // Check transfer offline
    std::string value = nTokenAmount.str();
    bool success = false;
    if (fCheckOutputs) {
        token.setCheckGasForCall(true);
        if (!token.transfer(address, value, success) || !success)
            throw JSONRPCError(RPC_MISC_ERROR, "Fail offline check for transfer token");
    }

    // Send token
    if (!token.transfer(address, value, success, true))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to transfer token");

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", token.getTxId());
    return result;
}

static UniValue qrc20transferfrom(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    QtumDGP qtumDGP(globalState.get(), fGettingValuesDGP);
    uint64_t blockGasLimit = qtumDGP.getBlockGasLimit(chainActive.Height());
    uint64_t minGasPrice = CAmount(qtumDGP.getMinGasPrice(chainActive.Height()));
    PriceOracle oracle;
    uint64_t oracleGasPrice;
    oracle.getPrice(oracleGasPrice);
    CAmount defaultGasPrice = (minGasPrice > DEFAULT_GAS_PRICE) ? minGasPrice : oracleGasPrice;
    Dgp dgp;
    CAmount gasPriceBuffer;
    dgp.calculateGasPriceBuffer(defaultGasPrice, gasPriceBuffer);
    CAmount nGasPrice = gasPriceBuffer + defaultGasPrice;
    uint64_t nGasLimit = DEFAULT_GAS_LIMIT_OP_SEND;
    bool fCheckOutputs = true;

    if (request.fHelp || request.params.size() < 4 || request.params.size() > 7)
        throw std::runtime_error(
            RPCHelpMan{
                "hrc20transferfrom",
                "\nSend token amount from selected address to a given address.\n",
                {
                    {"contractaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address."},
                    {"owneraddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The token owner hydra address."},
                    {"spenderaddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The token spender hydra address."},
                    {"receiveraddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The token receiver hydra address."},
                    {"amount", RPCArg::Type::STR, RPCArg::Optional::NO, "The amount of token to send. eg 0.1"},
                    {"gasLimit", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The gas limit, default: " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + ", max: " + i64tostr(blockGasLimit)},
                    {"checkOutputs", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Check outputs before send, default: true"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                    }},
                RPCExamples{
                    HelpExampleCli("hrc20transferfrom", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HhZThdumK8EFRX8MziWzvjCdiQWRt7Mxdz\" 0.1") + HelpExampleCli("hrc20transferfrom", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HhZThdumK8EFRX8MziWzvjCdiQWRt7Mxdz\" 0.1 " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + " true") + HelpExampleRpc("hrc20transferfrom", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HhZThdumK8EFRX8MziWzvjCdiQWRt7Mxdz\" 0.1") + HelpExampleRpc("hrc20transferfrom", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HhZThdumK8EFRX8MziWzvjCdiQWRt7Mxdz\" 0.1 " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + " true")},
            }
                .ToString());

    // Get mandatory parameters
    std::string contract = request.params[0].get_str();
    std::string owner = request.params[1].get_str();
    std::string spender = request.params[2].get_str();
    std::string receiver = request.params[3].get_str();
    std::string tokenAmount = request.params[4].get_str();

    // Get gas limit
    if (request.params.size() > 5) {
        nGasLimit = request.params[5].get_int64();
    }

    // Get check outputs flag
    if (request.params.size() > 6) {
        fCheckOutputs = request.params[6].get_bool();
    }

    // Set token parameters
    SendToken token(*locked_chain, pwallet);
    token.setAddress(contract);
    token.setSender(spender);
    token.setGasLimit(i64tostr(nGasLimit));

    // Get decimals
    uint32_t decimals;
    if (!token.decimals(decimals))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get decimals");

    // Get token amount to spend
    dev::s256 nTokenAmount;
    if (!ParseToken(decimals, tokenAmount, nTokenAmount))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get token amount");

    // Get token spender allowance
    std::string strAllowance;
    if (!token.allowance(owner, spender, strAllowance))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get allowance");

    // Check if allowance is enough to cover it
    dev::s256 allowance(strAllowance);
    if (allowance < nTokenAmount)
        throw JSONRPCError(RPC_MISC_ERROR, "Not enough token allowance");

    // Check transfer from offline
    std::string value = nTokenAmount.str();
    bool success = false;
    if (fCheckOutputs) {
        token.setCheckGasForCall(true);
        if (!token.transferFrom(owner, receiver, value, success) || !success)
            throw JSONRPCError(RPC_MISC_ERROR, "Fail offline check for spend token amount from address");
    }

    // Transfer allowed token amount
    if (!token.transferFrom(owner, receiver, value, success, true))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to spend token amount from address");

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", token.getTxId());
    return result;
}

static UniValue qrc20burn(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    QtumDGP qtumDGP(globalState.get(), fGettingValuesDGP);
    uint64_t blockGasLimit = qtumDGP.getBlockGasLimit(chainActive.Height());
    uint64_t minGasPrice = CAmount(qtumDGP.getMinGasPrice(chainActive.Height()));
    PriceOracle oracle;
    uint64_t oracleGasPrice;
    oracle.getPrice(oracleGasPrice);
    CAmount defaultGasPrice = (minGasPrice > DEFAULT_GAS_PRICE) ? minGasPrice : oracleGasPrice;
    Dgp dgp;
    CAmount gasPriceBuffer;
    dgp.calculateGasPriceBuffer(defaultGasPrice, gasPriceBuffer);
    CAmount nGasPrice = gasPriceBuffer + defaultGasPrice;
    uint64_t nGasLimit = DEFAULT_GAS_LIMIT_OP_SEND;
    bool fCheckOutputs = true;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 5)
        throw std::runtime_error(
            RPCHelpMan{
                "hrc20burn",
                "\nBurns token amount from owner address.\n",
                {
                    {"contractaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address."},
                    {"owneraddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The tokens owner hydra address."},
                    {"amount", RPCArg::Type::STR, RPCArg::Optional::NO, "The amount of tokens to burn. eg 0.1"},
                    {"gasLimit", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The gas limit, default: " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + ", max: " + i64tostr(blockGasLimit)},
                    {"checkOutputs", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Check outputs before send, default: true"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                    }},
                RPCExamples{
                    HelpExampleCli("hrc20burn", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1") + HelpExampleCli("hrc20burn", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + " true") + HelpExampleRpc("hrc20burn", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1") + HelpExampleRpc("hrc20burn", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + " true")},
            }
                .ToString());

    // Get mandatory parameters
    std::string contract = request.params[0].get_str();
    std::string owner = request.params[1].get_str();
    std::string tokenAmount = request.params[2].get_str();

    // Get gas limit
    if (request.params.size() > 3) {
        nGasLimit = request.params[3].get_int64();
    }

    // Get check outputs flag
    if (request.params.size() > 4) {
        fCheckOutputs = request.params[4].get_bool();
    }

    // Set token parameters
    SendToken token(*locked_chain, pwallet);
    token.setAddress(contract);
    token.setSender(owner);
    token.setGasLimit(i64tostr(nGasLimit));

    // Get decimals
    uint32_t decimals;
    if (!token.decimals(decimals))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get decimals");

    // Get token amount to burn
    dev::s256 nTokenAmount;
    if (!ParseToken(decimals, tokenAmount, nTokenAmount))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get token amount");

    // Get token owner balance
    std::string strBalance;
    if (!token.balanceOf(strBalance))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get balance");

    // Check if balance is enough to cover it
    dev::s256 balance(strBalance);
    if (balance < nTokenAmount)
        throw JSONRPCError(RPC_MISC_ERROR, "Not enough token balance");

    // Check burn offline
    std::string value = nTokenAmount.str();
    bool success = false;
    if (fCheckOutputs) {
        token.setCheckGasForCall(true);
        if (!token.burn(value, success) || !success)
            throw JSONRPCError(RPC_MISC_ERROR, "Fail offline check for burn token amount");
    }

    // Burn token amount
    if (!token.burn(value, success, true))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to burn token amount");

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", token.getTxId());
    return result;
}

static UniValue qrc20burnfrom(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    QtumDGP qtumDGP(globalState.get(), fGettingValuesDGP);
    uint64_t blockGasLimit = qtumDGP.getBlockGasLimit(chainActive.Height());
    uint64_t minGasPrice = CAmount(qtumDGP.getMinGasPrice(chainActive.Height()));
    PriceOracle oracle;
    uint64_t oracleGasPrice;
    oracle.getPrice(oracleGasPrice);
    CAmount defaultGasPrice = (minGasPrice > DEFAULT_GAS_PRICE) ? minGasPrice : oracleGasPrice;
    Dgp dgp;
    CAmount gasPriceBuffer;
    dgp.calculateGasPriceBuffer(defaultGasPrice, gasPriceBuffer);
    CAmount nGasPrice = gasPriceBuffer + defaultGasPrice;
    uint64_t nGasLimit = DEFAULT_GAS_LIMIT_OP_SEND;
    bool fCheckOutputs = true;

    if (request.fHelp || request.params.size() < 3 || request.params.size() > 6)
        throw std::runtime_error(
            RPCHelpMan{
                "hrc20burnfrom",
                "\nBurns token amount from a given address.\n",
                {
                    {"contractaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address."},
                    {"owneraddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The tokens owner hydra address."},
                    {"spenderaddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The token spender hydra address."},
                    {"amount", RPCArg::Type::STR, RPCArg::Optional::NO, "The amount of token to burn. eg 0.1"},
                    {"gasLimit", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The gas limit, default: " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + ", max: " + i64tostr(blockGasLimit)},
                    {"checkOutputs", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Check outputs before send, default: true"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                    }},
                RPCExamples{
                    HelpExampleCli("hrc20burnfrom", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1") + HelpExampleCli("hrc20burnfrom", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + " true") + HelpExampleRpc("hrc20burnfrom", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1") + HelpExampleRpc("hrc20burnfrom", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"HM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 " + i64tostr(DEFAULT_GAS_LIMIT_OP_SEND) + " true")},
            }
                .ToString());

    // Get mandatory parameters
    std::string contract = request.params[0].get_str();
    std::string owner = request.params[1].get_str();
    std::string spender = request.params[2].get_str();
    std::string tokenAmount = request.params[3].get_str();

    // Get gas limit
    if (request.params.size() > 4) {
        nGasLimit = request.params[4].get_int64();
    }

    // Get check outputs flag
    if (request.params.size() > 5) {
        fCheckOutputs = request.params[5].get_bool();
    }

    // Set token parameters
    SendToken token(*locked_chain, pwallet);
    token.setAddress(contract);
    token.setSender(spender);
    token.setGasLimit(i64tostr(nGasLimit));

    // Get decimals
    uint32_t decimals;
    if (!token.decimals(decimals))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get decimals");

    // Get token amount to burn
    dev::s256 nTokenAmount;
    if (!ParseToken(decimals, tokenAmount, nTokenAmount))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get token amount");

    // Get token spender allowance
    std::string strAllowance;
    if (!token.allowance(owner, spender, strAllowance))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get allowance");

    // Check if allowance is enough to cover it
    dev::s256 allowance(strAllowance);
    if (allowance < nTokenAmount)
        throw JSONRPCError(RPC_MISC_ERROR, "Not enough token allowance");

    // Check burn from offline
    std::string value = nTokenAmount.str();
    bool success = false;
    if (fCheckOutputs) {
        token.setCheckGasForCall(true);
        if (!token.burnFrom(owner, value, success, false) || !success)
            throw JSONRPCError(RPC_MISC_ERROR, "Fail offline check for burn token amount from address");
    }

    // Burn token amount
    if (!token.burnFrom(owner, value, success, true))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to burn token amount from address");

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", token.getTxId());
    return result;
}

static UniValue mintlydra(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    QtumDGP qtumDGP(globalState.get(), fGettingValuesDGP);
    uint64_t blockGasLimit = qtumDGP.getBlockGasLimit(chainActive.Height());
    uint64_t minGasPrice = CAmount(qtumDGP.getMinGasPrice(chainActive.Height()));
    PriceOracle oracle;
    uint64_t oracleGasPrice;
    oracle.getPrice(oracleGasPrice);
    CAmount defaultGasPrice = (minGasPrice > DEFAULT_GAS_PRICE) ? minGasPrice : oracleGasPrice;
    Dgp dgp;
    CAmount gasPriceBuffer;
    dgp.calculateGasPriceBuffer(defaultGasPrice, gasPriceBuffer);
    CAmount nGasPrice = gasPriceBuffer + defaultGasPrice;
    uint64_t nGasLimit = DEFAULT_GAS_LIMIT_OP_SEND;
    bool fCheckOutputs = true;

    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            RPCHelpMan{
                "mintlydra",
                "\nMint LYDRA (lock HYDRA) to a given wallet address.\nWARNING: Minting will happen only if address balance is above 5M gas!\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The hydra address that will lock HYDRA to mint LYDRA."},
                    {"lockAmount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "lockAmount (in HYDRA), default: 0, pass -1 for whole amount"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                    }},
                RPCExamples{
                    HelpExampleCli("mintlydra", "\"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\"") + HelpExampleCli("mintlydra", "\"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" 10")},
            }
                .ToString());

    UniValue senderaddress = request.params[0];
    CAmount lockAmount;
    if (request.params.size() > 1 && (request.params[1].getValStr() == std::to_string(COIN) || request.params[1].getValStr() == "-1")) lockAmount = -1;
    else lockAmount = request.params.size() > 1 ? AmountFromValue(request.params[1]) : 0;

    // Parse the sender address
    CTxDestination destSender = DecodeDestination(senderaddress.get_str());
    const CKeyID* pkhSender = boost::get<CKeyID>(&destSender);
    
    if (!pkhSender) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid contract sender address. Only P2PK and P2PKH allowed");
    }

    // Get the private key for the sender address
    CKey key;
    CKeyID keyID(*pkhSender);
    if (!pwallet->GetKey(keyID, key)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available for the sender address");
    }

    if (chainActive.Height() >= Params().GetConsensus().nLydraHeight) {
        std::string hex_senderaddress = pkhSender->GetReverseHex();
        Lydra lydraContract;
        std::string mintDatahex;
        lydraContract.getMintDatahex(mintDatahex);

        uint64_t locked_hydra_amount;
        lydraContract.getLockedHydraAmountPerAddress(hex_senderaddress, locked_hydra_amount);

        std::map<CTxDestination, CAmount> balances = pwallet->GetAddressBalances(*locked_chain);

        CAmount amount_to_lock;

        if (lockAmount == -1) {
            amount_to_lock = balances[DecodeDestination(senderaddress.get_str())];
            amount_to_lock -= locked_hydra_amount;
            amount_to_lock -= (nGasPrice * DEFAULT_GAS_LIMIT_OP_CREATE * 2);
        } else {
            if (lockAmount > balances[DecodeDestination(senderaddress.get_str())] - locked_hydra_amount - (nGasPrice * DEFAULT_GAS_LIMIT_OP_CREATE)) {
                amount_to_lock = balances[DecodeDestination(senderaddress.get_str())] - locked_hydra_amount - (nGasPrice * DEFAULT_GAS_LIMIT_OP_CREATE);
            } else {
                amount_to_lock = lockAmount;
            }
        }

        if (amount_to_lock > 0) {
            UniValue lydraParams(UniValue::VARR);
            lydraParams.push_back(HexStr(Params().GetConsensus().lydraAddress));
            lydraParams.push_back(mintDatahex);
            lydraParams.push_back(0);
            lydraParams.push_back(nGasLimit);
            lydraParams.push_back(senderaddress);

            auto mint_ret = SendToContract(*locked_chain, pwallet, lydraParams, amount_to_lock);
            if (mint_ret.isObject()) updateLydraLockedCache(amount_to_lock, hex_senderaddress, true);
            return mint_ret;
        } else {
            throw JSONRPCError(RPC_WALLET_ERROR, "Address balance is below 5M gas!");
        }
    } else {
        throw JSONRPCError(RPC_TYPE_ERROR, "LYDRA is not activated yet!");
    }
}

static UniValue burnlydra(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    QtumDGP qtumDGP(globalState.get(), fGettingValuesDGP);
    uint64_t blockGasLimit = qtumDGP.getBlockGasLimit(chainActive.Height());
    uint64_t minGasPrice = CAmount(qtumDGP.getMinGasPrice(chainActive.Height()));
    PriceOracle oracle;
    uint64_t oracleGasPrice;
    oracle.getPrice(oracleGasPrice);
    CAmount defaultGasPrice = (minGasPrice > DEFAULT_GAS_PRICE) ? minGasPrice : oracleGasPrice;
    Dgp dgp;
    CAmount gasPriceBuffer;
    dgp.calculateGasPriceBuffer(defaultGasPrice, gasPriceBuffer);
    CAmount nGasPrice = gasPriceBuffer + defaultGasPrice;
    uint64_t nGasLimit = DEFAULT_GAS_LIMIT_OP_SEND;
    bool fCheckOutputs = true;

    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            RPCHelpMan{
                "burnlydra",
                "\nBurn LYDRA (unlock HYDRA) from a given wallet address.\nWARNING: Equal amount of HYDRA will be unlocked to this address only if it contains any LYDRA balance!\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The hydra address that will burn LYDRA to unlock HYDRA."},
                    {"unlockAmount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "unlockAmount (in HYDRA), default: 0, pass -1 for whole amount"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                    }},
                RPCExamples{
                    HelpExampleCli("burnlydra", "\"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\"") + HelpExampleCli("burnlydra", "\"HX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" 10")},
            }
                .ToString());

    UniValue senderaddress = request.params[0];
    CAmount unlockAmount;
    if (request.params.size() > 1 && (request.params[1].getValStr() == std::to_string(COIN) || request.params[1].getValStr() == "-1")) unlockAmount = -1;
    else unlockAmount = request.params.size() > 1 ? AmountFromValue(request.params[1]) : 0;

    // Parse the sender address
    CTxDestination destSender = DecodeDestination(senderaddress.get_str());
    const CKeyID* pkhSender = boost::get<CKeyID>(&destSender);
    
    if (!pkhSender) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid contract sender address. Only P2PK and P2PKH allowed");
    }

    // Get the private key for the sender address
    CKey key;
    CKeyID keyID(*pkhSender);
    if (!pwallet->GetKey(keyID, key)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available for the sender address");
    }

    if (chainActive.Height() >= Params().GetConsensus().nLydraHeight) {
        Lydra lydraContract;
        std::string burnDatahex{};
        lydraContract.getBurnDatahex(burnDatahex, unlockAmount);

        UniValue lydraParams(UniValue::VARR);
        lydraParams.push_back(HexStr(Params().GetConsensus().lydraAddress));
        lydraParams.push_back(burnDatahex);
        lydraParams.push_back(0);
        lydraParams.push_back(nGasLimit);
        lydraParams.push_back(senderaddress);

        auto burn_ret = SendToContract(*locked_chain, pwallet, lydraParams, -1);

        if (burn_ret.isObject()) {
            if (unlockAmount == -1)
                clearLydraLockedCache(pkhSender->GetReverseHex());
            else
                updateLydraLockedCache(unlockAmount, pkhSender->GetReverseHex(), false);
        }

        return burn_ret;
    } else {
        throw JSONRPCError(RPC_TYPE_ERROR, "LYDRA is not activated yet!");
    }
}

UniValue abortrescan(const JSONRPCRequest& request); // in rpcdump.cpp
UniValue dumpprivkey(const JSONRPCRequest& request); // in rpcdump.cpp
UniValue importprivkey(const JSONRPCRequest& request);
UniValue importaddress(const JSONRPCRequest& request);
UniValue importpubkey(const JSONRPCRequest& request);
UniValue dumpwallet(const JSONRPCRequest& request);
UniValue importwallet(const JSONRPCRequest& request);
UniValue importprunedfunds(const JSONRPCRequest& request);
UniValue removeprunedfunds(const JSONRPCRequest& request);
UniValue importmulti(const JSONRPCRequest& request);

// clang-format off
static const CRPCCommand commands[] =
{ //  category              name                                actor (function)                argNames
    //  --------------------- ------------------------          -----------------------         ----------
    { "generating",         "generate",                         &generate,                      {"nblocks","maxtries"} },
    { "hidden",             "resendwallettransactions",         &resendwallettransactions,      {} },
    { "rawtransactions",    "fundrawtransaction",               &fundrawtransaction,            {"hexstring","options","iswitness"} },
    { "wallet",             "abandontransaction",               &abandontransaction,            {"txid"} },
    { "wallet",             "abortrescan",                      &abortrescan,                   {} },
    { "wallet",             "addmultisigaddress",               &addmultisigaddress,            {"nrequired","keys","label","address_type"} },
    { "wallet",             "backupwallet",                     &backupwallet,                  {"destination"} },
    { "wallet",             "bumpfee",                          &bumpfee,                       {"txid", "options"} },
    { "wallet",             "createwallet",                     &createwallet,                  {"wallet_name", "disable_private_keys", "blank"} },
    { "wallet",             "dumpprivkey",                      &dumpprivkey,                   {"address"}  },
    { "wallet",             "dumpwallet",                       &dumpwallet,                    {"filename"} },
    { "wallet",             "encryptwallet",                    &encryptwallet,                 {"passphrase"} },
    { "wallet",             "getaddressesbylabel",              &getaddressesbylabel,           {"label"} },
    { "wallet",             "getaddressinfo",                   &getaddressinfo,                {"address"} },
    { "wallet",             "getbalance",                       &getbalance,                    {"dummy","minconf","include_watchonly"} },
    { "wallet",             "getbalanceofaddress",              &getbalanceofaddress,           {"address"} },
    { "wallet",             "getnewaddress",                    &getnewaddress,                 {"label","address_type"} },
    { "wallet",             "getrawchangeaddress",              &getrawchangeaddress,           {"address_type"} },
    { "wallet",             "getreceivedbyaddress",             &getreceivedbyaddress,          {"address","minconf"} },
    { "wallet",             "getreceivedbylabel",               &getreceivedbylabel,            {"label","minconf"} },
    { "wallet",             "gettransaction",                   &gettransaction,                {"txid","include_watchonly", "waitconf"} },
    { "wallet",             "getunconfirmedbalance",            &getunconfirmedbalance,         {} },
    { "wallet",             "getwalletinfo",                    &getwalletinfo,                 {} },
    { "wallet",             "importaddress",                    &importaddress,                 {"address","label","rescan","p2sh"} },
    { "wallet",             "importmulti",                      &importmulti,                   {"requests","options"} },
    { "wallet",             "importprivkey",                    &importprivkey,                 {"privkey","label","rescan"} },
    { "wallet",             "importprunedfunds",                &importprunedfunds,             {"rawtransaction","txoutproof"} },
    { "wallet",             "importpubkey",                     &importpubkey,                  {"pubkey","label","rescan"} },
    { "wallet",             "importwallet",                     &importwallet,                  {"filename"} },
    { "wallet",             "keypoolrefill",                    &keypoolrefill,                 {"newsize"} },
    { "wallet",             "listaddressgroupings",             &listaddressgroupings,          {} },
    { "wallet",             "listlabels",                       &listlabels,                    {"purpose"} },
    { "wallet",             "listlockunspent",                  &listlockunspent,               {} },
    { "wallet",             "listreceivedbyaddress",            &listreceivedbyaddress,         {"minconf","include_empty","include_watchonly","address_filter"} },
    { "wallet",             "listreceivedbylabel",              &listreceivedbylabel,           {"minconf","include_empty","include_watchonly"} },
    { "wallet",             "listsinceblock",                   &listsinceblock,                {"blockhash","target_confirmations","include_watchonly","include_removed"} },
    { "wallet",             "listtransactions",                 &listtransactions,              {"label|dummy","count","skip","include_watchonly"} },
    { "wallet",             "listunspent",                      &listunspent,                   {"minconf","maxconf","addresses","include_unsafe","query_options"} },
    { "wallet",             "listwalletdir",                    &listwalletdir,                 {} },
    { "wallet",             "listwallets",                      &listwallets,                   {} },
    { "wallet",             "loadwallet",                       &loadwallet,                    {"filename"} },
    { "wallet",             "lockunspent",                      &lockunspent,                   {"unlock","transactions"} },
    { "wallet",             "removeprunedfunds",                &removeprunedfunds,             {"txid"} },
    { "wallet",             "rescanblockchain",                 &rescanblockchain,              {"start_height", "stop_height"} },
    { "wallet",             "sendmany",                         &sendmany,                      {"dummy","amounts","minconf","comment","subtractfeefrom","replaceable","conf_target","estimate_mode"} },
    { "wallet",             "sendmanywithdupes",                &sendmanywithdupes,             {"fromaccount","amounts","minconf","comment","subtractfeefrom"} },
    { "wallet",             "sendtoaddress",                    &sendtoaddress,                 {"address","amount","comment","comment_to","subtractfeefromamount","replaceable","conf_target","estimate_mode","senderAddress","changeToSender"} },
    { "wallet",             "splitutxosforaddress",             &splitutxosforaddress,          {"address","minValue","maxValue","maxOutputs"} },
    { "wallet",             "sethdseed",                        &sethdseed,                     {"newkeypool","seed"} },
    { "wallet",             "setlabel",                         &setlabel,                      {"address","label"} },
    { "wallet",             "settxfee",                         &settxfee,                      {"amount"} },
    { "wallet",             "signmessage",                      &signmessage,                   {"address","message"} },
    { "wallet",             "signrawtransactionwithwallet",     &signrawtransactionwithwallet,  {"hexstring","prevtxs","sighashtype"} },
    { "wallet",             "signrawsendertransactionwithwallet", &signrawsendertransactionwithwallet,  {"hexstring","sighashtype"} },
    { "wallet",             "unloadwallet",                     &unloadwallet,                  {"wallet_name"} },
    { "wallet",             "walletcreatefundedpsbt",           &walletcreatefundedpsbt,        {"inputs","outputs","locktime","options","bip32derivs"} },
    { "wallet",             "walletlock",                       &walletlock,                    {} },
    { "wallet",             "walletpassphrase",                 &walletpassphrase,              {"passphrase","timeout","stakingonly"} },
    { "wallet",             "walletpassphrasechange",           &walletpassphrasechange,        {"oldpassphrase","newpassphrase"} },
    { "wallet",             "walletprocesspsbt",                &walletprocesspsbt,             {"psbt","sign","sighashtype","bip32derivs"} },
    { "wallet",             "reservebalance",                   &reservebalance,                {"reserve", "amount"} },
    { "wallet",             "createcontract",                   &createcontract,                {"bytecode", "gasLimit", "senderAddress", "broadcast", "changeToSender"} },
    { "wallet",             "sendtocontract",                   &sendtocontract,                {"contractaddress", "bytecode", "amount", "gasLimit", "senderAddress", "broadcast", "changeToSender"} },
    { "wallet",             "removedelegationforaddress",       &removedelegationforaddress,    {"address", "gasLimit"} },
    { "wallet",             "setdelegateforaddress",            &setdelegateforaddress,         {"staker", "fee", "address", "gasLimit"} },
    { "wallet",             "addsuperstakeraddress",            &addsuperstakeraddress,         {"address", "stakingminutxovalue", "stakingminfee", "allow", "exclude"} },
    { "wallet",             "removesuperstakeraddress",         &removesuperstakeraddress,       {"address"} },
    { "wallet",             "setsuperstakervaluesforaddress",   &setsuperstakervaluesforaddress, {"address", "stakingminutxovalue", "stakingminfee", "allow", "exclude"} },
    { "wallet",             "listsuperstakercustomvalues",             &listsuperstakercustomvalues,          {} },
    { "wallet",             "listsuperstakervaluesforaddress",         &listsuperstakervaluesforaddress,      {"address"} },
    { "wallet",             "removesuperstakervaluesforaddress",       &removesuperstakervaluesforaddress,    {"address"} },
    { "wallet",             "hrc20approve",                    &qrc20approve,                   {"contractaddress", "owneraddress", "spenderaddress", "amount", "gasLimit", "checkOutputs"} },
    { "wallet",             "hrc20transfer",                   &qrc20transfer,                  {"contractaddress", "owneraddress", "addressto", "amount", "gasLimit", "checkOutputs"} },
    { "wallet",             "hrc20transferfrom",               &qrc20transferfrom,              {"contractaddress", "owneraddress", "spenderaddress", "receiveraddress", "amount", "gasLimit", "checkOutputs"} },
    { "wallet",             "hrc20burn",                       &qrc20burn,                      {"contractaddress", "owneraddress", "amount", "gasLimit", "checkOutputs"} },
    { "wallet",             "hrc20burnfrom",                   &qrc20burnfrom,                  {"contractaddress", "owneraddress", "spenderaddress", "amount", "gasLimit", "checkOutputs"} },
    { "wallet",             "mintlydra",                       &mintlydra,                      {"address", "lockAmount"} },
    { "wallet",             "burnlydra",                       &burnlydra,                      {"address", "unlockAmount"} },
};
// clang-format on

void RegisterWalletRPCCommands(CRPCTable& t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
