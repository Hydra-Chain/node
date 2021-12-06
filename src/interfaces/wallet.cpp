// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/wallet.h>

#include <amount.h>
#include <chain.h>
#include <consensus/validation.h>
#include <init.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <net.h>
#include <policy/feerate.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <rpc/server.h>
#include <scheduler.h>
#include <script/ismine.h>
#include <script/standard.h>
#include <support/allocators/secure.h>
#include <sync.h>
#include <timedata.h>
#include <ui_interface.h>
#include <uint256.h>
#include <util/system.h>
#include <validation.h>
#include <wallet/feebumper.h>
#include <wallet/fees.h>
#include <wallet/rpcwallet.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>
#include <key_io.h>
#include <qtum/qtumdelegation.h>
#include <miner.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>

namespace interfaces {
namespace {

class PendingWalletTxImpl : public PendingWalletTx
{
public:
    explicit PendingWalletTxImpl(CWallet& wallet) : m_wallet(wallet), m_key(&wallet) {}

    const CTransaction& get() override { return *m_tx; }

    int64_t getVirtualSize() override { return GetVirtualTransactionSize(*m_tx); }

    bool commit(WalletValueMap value_map,
        WalletOrderForm order_form,
        std::string& reject_reason) override
    {
        auto locked_chain = m_wallet.chain().lock();
        LOCK(m_wallet.cs_wallet);
        CValidationState state;
        if (!m_wallet.CommitTransaction(m_tx, std::move(value_map), std::move(order_form), m_key, g_connman.get(), state)) {
            reject_reason = state.GetRejectReason();
            return false;
        }
        return true;
    }

    CTransactionRef m_tx;
    CWallet& m_wallet;
    CReserveKey m_key;
};

//! Construct wallet tx struct.
WalletTx MakeWalletTx(interfaces::Chain::Lock& locked_chain, CWallet& wallet, const CWalletTx& wtx)
{
    WalletTx result;
    result.tx = wtx.tx;
    result.txin_is_mine.reserve(wtx.tx->vin.size());
    for (const auto& txin : wtx.tx->vin) {
        result.txin_is_mine.emplace_back(wallet.IsMine(txin));
    }
    result.txout_is_mine.reserve(wtx.tx->vout.size());
    result.txout_address.reserve(wtx.tx->vout.size());
    result.txout_address_is_mine.reserve(wtx.tx->vout.size());
    for (const auto& txout : wtx.tx->vout) {
        result.txout_is_mine.emplace_back(wallet.IsMine(txout));
        result.txout_address.emplace_back();
        result.txout_address_is_mine.emplace_back(ExtractDestination(txout.scriptPubKey, result.txout_address.back()) ?
                                                      IsMine(wallet, result.txout_address.back()) :
                                                      ISMINE_NO);
    }
    result.credit = wtx.GetCredit(locked_chain, ISMINE_ALL);
    result.debit = wtx.GetDebit(ISMINE_ALL);
    result.change = wtx.GetChange();
    result.time = wtx.GetTxTime();
    result.value_map = wtx.mapValue;
    result.is_coinbase = wtx.IsCoinBase();
    result.is_coinstake = wtx.IsCoinStake();
    result.is_in_main_chain = wtx.IsInMainChain(locked_chain);
    result.has_create_or_call = wtx.tx->HasCreateOrCall();
    if(result.has_create_or_call)
    {
        CTxDestination tx_sender_address;
        if(wtx.tx && wtx.tx->vin.size() > 0 && wallet.mapWallet.find(wtx.tx->vin[0].prevout.hash) != wallet.mapWallet.end() &&
                ExtractDestination(wallet.mapWallet.at(wtx.tx->vin[0].prevout.hash).tx->vout[wtx.tx->vin[0].prevout.n].scriptPubKey, tx_sender_address)) {
            result.tx_sender_key = GetKeyForDestination(wallet, tx_sender_address);
        }

        for(CTxDestination address : result.txout_address) {
            result.txout_keys.emplace_back(GetKeyForDestination(wallet, address));
        }
    }
    return result;
}

//! Construct wallet tx status struct.
WalletTxStatus MakeWalletTxStatus(interfaces::Chain::Lock& locked_chain, const CWalletTx& wtx)
{
    LockAnnotation lock(::cs_main); // Temporary, for CheckFinalTx below. Removed in upcoming commit.

    WalletTxStatus result;
    auto mi = ::mapBlockIndex.find(wtx.hashBlock);
    CBlockIndex* block = mi != ::mapBlockIndex.end() ? mi->second : nullptr;
    result.block_height = (block ? block->nHeight : std::numeric_limits<int>::max());
    result.blocks_to_maturity = wtx.GetBlocksToMaturity(locked_chain);
    result.depth_in_main_chain = wtx.GetDepthInMainChain(locked_chain);
    result.time_received = wtx.nTimeReceived;
    result.lock_time = wtx.tx->nLockTime;
    result.is_final = CheckFinalTx(*wtx.tx);
    result.is_trusted = wtx.IsTrusted(locked_chain);
    result.is_abandoned = wtx.isAbandoned();
    result.is_coinbase = wtx.IsCoinBase();
    result.is_coinstake = wtx.IsCoinStake();
    result.is_in_main_chain = wtx.IsInMainChain(locked_chain);
    return result;
}

//! Construct wallet TxOut struct.
WalletTxOut MakeWalletTxOut(interfaces::Chain::Lock& locked_chain,
    CWallet& wallet,
    const CWalletTx& wtx,
    int n,
    int depth) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    WalletTxOut result;
    result.txout = wtx.tx->vout[n];
    result.time = wtx.GetTxTime();
    result.depth_in_main_chain = depth;
    result.is_spent = wallet.IsSpent(locked_chain, wtx.GetHash(), n);
    return result;
}

//! Construct token info.
CTokenInfo MakeTokenInfo(const TokenInfo& token)
{
    CTokenInfo result;
    result.strContractAddress = token.contract_address;
    result.strTokenName = token.token_name;
    result.strTokenSymbol = token.token_symbol;
    result.nDecimals = token.decimals;
    result.strSenderAddress = token.sender_address;
    result.nCreateTime = token.time;
    result.blockHash = token.block_hash;
    result.blockNumber = token.block_number;
    return result;
}

//! Construct wallet token info.
TokenInfo MakeWalletTokenInfo(const CTokenInfo& token)
{
    TokenInfo result;
    result.contract_address = token.strContractAddress;
    result.token_name = token.strTokenName;
    result.token_symbol = token.strTokenSymbol;
    result.decimals = token.nDecimals;
    result.sender_address = token.strSenderAddress;
    result.time = token.nCreateTime;
    result.block_hash = token.blockHash;
    result.block_number = token.blockNumber;
    result.hash = token.GetHash();
    return result;
}

//! Construct token transaction.
CTokenTx MakeTokenTx(const TokenTx& tokenTx)
{
    CTokenTx result;
    result.strContractAddress = tokenTx.contract_address;
    result.strSenderAddress = tokenTx.sender_address;
    result.strReceiverAddress = tokenTx.receiver_address;
    result.nValue = tokenTx.value;
    result.transactionHash = tokenTx.tx_hash;
    result.nCreateTime = tokenTx.time;
    result.blockHash = tokenTx.block_hash;
    result.blockNumber = tokenTx.block_number;
    result.strLabel = tokenTx.label;
    return result;
}

//! Construct wallet token transaction.
TokenTx MakeWalletTokenTx(const CTokenTx& tokenTx)
{
    TokenTx result;
    result.contract_address = tokenTx.strContractAddress;
    result.sender_address = tokenTx.strSenderAddress;
    result.receiver_address = tokenTx.strReceiverAddress;
    result.value = tokenTx.nValue;
    result.tx_hash = tokenTx.transactionHash;
    result.time = tokenTx.nCreateTime;
    result.block_hash = tokenTx.blockHash;
    result.block_number = tokenTx.blockNumber;
    result.label = tokenTx.strLabel;
    result.hash = tokenTx.GetHash();
    return result;
}

ContractBookData MakeContractBook(const std::string& id, const CContractBookData& data)
{
    ContractBookData result;
    result.address = id;
    result.name = data.name;
    result.abi = data.abi;
    return result;
}

uint160 StringToKeyId(const std::string& strAddress)
{
    CTxDestination dest = DecodeDestination(strAddress);
    const CKeyID *keyID = boost::get<CKeyID>(&dest);
    if(keyID)
    {
        return uint160(*keyID);
    }
    return uint160();
}

std::string KeyIdToString(const uint160& keyID)
{
    return EncodeDestination(CKeyID(keyID));
}

std::vector<uint160> StringToKeyIdList(const std::vector<std::string>& listAddress)
{
    std::vector<uint160> ret;
    for(auto address : listAddress)
    {
        ret.push_back(StringToKeyId(address));
    }
    return ret;
}

std::vector<std::string> KeyIdToStringList(const std::vector<uint160>& listKeyID)
{
    std::vector<std::string> ret;
    for(auto keyId : listKeyID)
    {
        ret.push_back(KeyIdToString(keyId));
    }
    return ret;
}

//! Construct delegation info.
CDelegationInfo MakeDelegationInfo(const DelegationInfo& delegation)
{
    CDelegationInfo result;
    result.delegateAddress = StringToKeyId(delegation.delegate_address);
    result.stakerAddress = StringToKeyId(delegation.staker_address);
    result.strStakerName = delegation.staker_name;
    result.nFee = delegation.fee;
    result.nCreateTime = delegation.time;
    result.blockNumber = delegation.block_number;
    result.createTxHash = delegation.create_tx_hash;
    result.removeTxHash = delegation.remove_tx_hash;
    return result;
}

//! Construct wallet delegation info.
DelegationInfo MakeWalletDelegationInfo(const CDelegationInfo& delegation)
{
    DelegationInfo result;
    result.delegate_address = KeyIdToString(delegation.delegateAddress);
    result.staker_address = KeyIdToString(delegation.stakerAddress);
    result.staker_name = delegation.strStakerName;
    result.fee = delegation.nFee;
    result.time = delegation.nCreateTime;
    result.block_number = delegation.blockNumber;
    result.time = delegation.nCreateTime;
    result.create_tx_hash = delegation.createTxHash;
    result.remove_tx_hash = delegation.removeTxHash;
    result.hash = delegation.GetHash();
    return result;
}

//! Construct super staker info.
CSuperStakerInfo MakeSuperStakerInfo(const SuperStakerInfo& superStaker)
{
    CSuperStakerInfo result;
    result.stakerAddress = StringToKeyId(superStaker.staker_address);
    result.strStakerName = superStaker.staker_name;
    result.nMinFee = superStaker.min_fee;
    result.nCreateTime = superStaker.time;
    result.fCustomConfig = superStaker.custom_config;
    result.nMinDelegateUtxo = superStaker.min_delegate_utxo;
    result.delegateAddressList = StringToKeyIdList(superStaker.delegate_address_list);
    result.nDelegateAddressType = superStaker.delegate_address_type;
    return result;
}

//! Construct wallet super staker info.
SuperStakerInfo MakeWalletSuperStakerInfo(const CSuperStakerInfo& superStaker)
{
    SuperStakerInfo result;
    result.staker_address = KeyIdToString(superStaker.stakerAddress);
    result.staker_name = superStaker.strStakerName;
    result.min_fee = superStaker.nMinFee;
    result.time = superStaker.nCreateTime;
    result.custom_config = superStaker.fCustomConfig;
    result.min_delegate_utxo = superStaker.nMinDelegateUtxo;
    result.delegate_address_list = KeyIdToStringList(superStaker.delegateAddressList);
    result.delegate_address_type = superStaker.nDelegateAddressType;
    result.hash = superStaker.GetHash();
    return result;
}

//! Construct wallet delegation staker info.
DelegationStakerInfo MakeWalletDelegationStakerInfo(interfaces::Chain::Lock& locked_chain, CWallet& wallet, const uint160& id, const Delegation& delegation)
{
    DelegationStakerInfo result;
    result.delegate_address = EncodeDestination(CKeyID(id));
    result.staker_address = EncodeDestination(CKeyID(delegation.staker));
    result.PoD = HexStr(delegation.PoD);
    result.fee = delegation.fee;
    result.time = locked_chain.getBlockTime(delegation.blockHeight);
    result.block_number = delegation.blockHeight;
    std::map<uint160, CAmount>::iterator it = wallet.m_delegations_weight.find(id);
    if(it != wallet.m_delegations_weight.end())
    {
        result.weight = it->second;
    }
    result.hash = id;
    return result;
}


bool TokenTxStatus(interfaces::Chain::Lock& locked_chain, CWallet& wallet, const uint256& txid, int& block_number, bool& in_mempool, int& num_blocks)
{
    auto mi = wallet.mapTokenTx.find(txid);
    if (mi == wallet.mapTokenTx.end()) {
        return false;
    }
    block_number = mi->second.blockNumber;
    auto it = wallet.mapWallet.find(mi->second.transactionHash); 
    if(it != wallet.mapWallet.end())
    {
        in_mempool = it->second.InMempool();
    }
    if (Optional<int> height = locked_chain.getHeight()) {
        num_blocks = *height;
    } else {
        num_blocks = -1;
    }
    return true;
}

class WalletImpl : public Wallet
{
public:
    explicit WalletImpl(const std::shared_ptr<CWallet>& wallet) : m_wallet(wallet) {}

    bool encryptWallet(const SecureString& wallet_passphrase) override
    {
        return m_wallet->EncryptWallet(wallet_passphrase);
    }
    bool isCrypted() override { return m_wallet->IsCrypted(); }
    bool lock() override { return m_wallet->Lock(); }
    bool unlock(const SecureString& wallet_passphrase) override { return m_wallet->Unlock(wallet_passphrase); }
    bool isLocked() override { return m_wallet->IsLocked(); }
    bool changeWalletPassphrase(const SecureString& old_wallet_passphrase,
        const SecureString& new_wallet_passphrase) override
    {
        return m_wallet->ChangeWalletPassphrase(old_wallet_passphrase, new_wallet_passphrase);
    }
    void abortRescan() override { m_wallet->AbortRescan(); }
    bool backupWallet(const std::string& filename) override { return m_wallet->BackupWallet(filename); }
    std::string getWalletName() override { return m_wallet->GetName(); }
    bool getKeyFromPool(bool internal, CPubKey& pub_key) override
    {
        return m_wallet->GetKeyFromPool(pub_key, internal);
    }
    bool getPubKey(const CKeyID& address, CPubKey& pub_key) override { return m_wallet->GetPubKey(address, pub_key); }
    bool getPrivKey(const CKeyID& address, CKey& key) override { return m_wallet->GetKey(address, key); }
    bool isSpendable(const CTxDestination& dest) override { return IsMine(*m_wallet, dest) & ISMINE_SPENDABLE; }
    bool haveWatchOnly() override { return m_wallet->HaveWatchOnly(); };
    bool setAddressBook(const CTxDestination& dest, const std::string& name, const std::string& purpose) override
    {
        return m_wallet->SetAddressBook(dest, name, purpose);
    }
    bool delAddressBook(const CTxDestination& dest) override
    {
        return m_wallet->DelAddressBook(dest);
    }
    bool getAddress(const CTxDestination& dest,
        std::string* name,
        isminetype* is_mine,
        std::string* purpose) override
    {
        LOCK(m_wallet->cs_wallet);
        auto it = m_wallet->mapAddressBook.find(dest);
        if (it == m_wallet->mapAddressBook.end()) {
            return false;
        }
        if (name) {
            *name = it->second.name;
        }
        if (is_mine) {
            *is_mine = IsMine(*m_wallet, dest);
        }
        if (purpose) {
            *purpose = it->second.purpose;
        }
        return true;
    }
    std::vector<WalletAddress> getAddresses() override
    {
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletAddress> result;
        for (const auto& item : m_wallet->mapAddressBook) {
            result.emplace_back(item.first, IsMine(*m_wallet, item.first), item.second.name, item.second.purpose);
        }
        return result;
    }
    void learnRelatedScripts(const CPubKey& key, OutputType type) override { m_wallet->LearnRelatedScripts(key, type); }
    bool addDestData(const CTxDestination& dest, const std::string& key, const std::string& value) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->AddDestData(dest, key, value);
    }
    bool eraseDestData(const CTxDestination& dest, const std::string& key) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->EraseDestData(dest, key);
    }
    std::vector<std::string> getDestValues(const std::string& prefix) override
    {
        LOCK(m_wallet->cs_wallet);
        return m_wallet->GetDestValues(prefix);
    }
    void lockCoin(const COutPoint& output) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->LockCoin(output);
    }
    void unlockCoin(const COutPoint& output) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->UnlockCoin(output);
    }
    bool isLockedCoin(const COutPoint& output) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->IsLockedCoin(output.hash, output.n);
    }
    void listLockedCoins(std::vector<COutPoint>& outputs) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->ListLockedCoins(outputs);
    }
    std::unique_ptr<PendingWalletTx> createTransaction(const std::vector<CRecipient>& recipients,
        const CCoinControl& coin_control,
        bool sign,
        int& change_pos,
        CAmount& fee,
        std::string& fail_reason) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        auto pending = MakeUnique<PendingWalletTxImpl>(*m_wallet);
        if (!m_wallet->CreateTransaction(*locked_chain, recipients, pending->m_tx, pending->m_key, fee, change_pos,
                fail_reason, coin_control, sign)) {
            return {};
        }
        return std::move(pending);
    }
    bool transactionCanBeAbandoned(const uint256& txid) override { return m_wallet->TransactionCanBeAbandoned(txid); }
    bool abandonTransaction(const uint256& txid) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->AbandonTransaction(*locked_chain, txid);
    }
    bool transactionCanBeBumped(const uint256& txid) override
    {
        return feebumper::TransactionCanBeBumped(m_wallet.get(), txid);
    }
    bool createBumpTransaction(const uint256& txid,
        const CCoinControl& coin_control,
        CAmount total_fee,
        std::vector<std::string>& errors,
        CAmount& old_fee,
        CAmount& new_fee,
        CMutableTransaction& mtx) override
    {
        return feebumper::CreateTransaction(m_wallet.get(), txid, coin_control, total_fee, errors, old_fee, new_fee, mtx) ==
               feebumper::Result::OK;
    }
    bool signBumpTransaction(CMutableTransaction& mtx) override { return feebumper::SignTransaction(m_wallet.get(), mtx); }
    bool commitBumpTransaction(const uint256& txid,
        CMutableTransaction&& mtx,
        std::vector<std::string>& errors,
        uint256& bumped_txid) override
    {
        return feebumper::CommitTransaction(m_wallet.get(), txid, std::move(mtx), errors, bumped_txid) ==
               feebumper::Result::OK;
    }
    CTransactionRef getTx(const uint256& txid) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi != m_wallet->mapWallet.end()) {
            return mi->second.tx;
        }
        return {};
    }
    WalletTx getWalletTx(const uint256& txid) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi != m_wallet->mapWallet.end()) {
            return MakeWalletTx(*locked_chain, *m_wallet, mi->second);
        }
        return {};
    }
    std::vector<WalletTx> getWalletTxs() override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletTx> result;
        result.reserve(m_wallet->mapWallet.size());
        for (const auto& entry : m_wallet->mapWallet) {
            result.emplace_back(MakeWalletTx(*locked_chain, *m_wallet, entry.second));
        }
        return result;
    }
    bool tryGetTxStatus(const uint256& txid,
        interfaces::WalletTxStatus& tx_status,
        int& num_blocks,
        int64_t& block_time) override
    {
        auto locked_chain = m_wallet->chain().lock(true /* try_lock */);
        if (!locked_chain) {
            return false;
        }
        TRY_LOCK(m_wallet->cs_wallet, locked_wallet);
        if (!locked_wallet) {
            return false;
        }
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi == m_wallet->mapWallet.end()) {
            return false;
        }
        if (Optional<int> height = locked_chain->getHeight()) {
            num_blocks = *height;
            block_time = locked_chain->getBlockTime(*height);
        } else {
            num_blocks = -1;
            block_time = -1;
        }
        tx_status = MakeWalletTxStatus(*locked_chain, mi->second);
        return true;
    }
    WalletTx getWalletTxDetails(const uint256& txid,
        WalletTxStatus& tx_status,
        WalletOrderForm& order_form,
        bool& in_mempool,
        int& num_blocks) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        auto mi = m_wallet->mapWallet.find(txid);
        if (mi != m_wallet->mapWallet.end()) {
            num_blocks = locked_chain->getHeight().get_value_or(-1);
            in_mempool = mi->second.InMempool();
            order_form = mi->second.vOrderForm;
            tx_status = MakeWalletTxStatus(*locked_chain, mi->second);
            return MakeWalletTx(*locked_chain, *m_wallet, mi->second);
        }
        return {};
    }
    WalletBalances getBalances() override
    {
        WalletBalances result;
        result.balance = m_wallet->GetBalance();
        result.unconfirmed_balance = m_wallet->GetUnconfirmedBalance();
        result.immature_balance = m_wallet->GetImmatureBalance();
        result.stake = m_wallet->GetStake();
        result.have_watch_only = m_wallet->HaveWatchOnly();
        if (result.have_watch_only) {
            result.watch_only_balance = m_wallet->GetBalance(ISMINE_WATCH_ONLY);
            result.unconfirmed_watch_only_balance = m_wallet->GetUnconfirmedWatchOnlyBalance();
            result.immature_watch_only_balance = m_wallet->GetImmatureWatchOnlyBalance();
            result.watch_only_stake = m_wallet->GetWatchOnlyStake();
        }
        return result;
    }
    bool tryGetBalances(WalletBalances& balances, int& num_blocks) override
    {
        auto locked_chain = m_wallet->chain().lock(true /* try_lock */);
        if (!locked_chain) return false;
        TRY_LOCK(m_wallet->cs_wallet, locked_wallet);
        if (!locked_wallet) {
            return false;
        }
        balances = getBalances();
        num_blocks = locked_chain->getHeight().get_value_or(-1);
        return true;
    }
    CAmount getBalance() override { return m_wallet->GetBalance(); }
    CAmount getAvailableBalance(const CCoinControl& coin_control) override
    {
        return m_wallet->GetAvailableBalance(&coin_control);
    }
    isminetype txinIsMine(const CTxIn& txin) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->IsMine(txin);
    }
    isminetype txoutIsMine(const CTxOut& txout) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->IsMine(txout);
    }
    CAmount getDebit(const CTxIn& txin, isminefilter filter) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->GetDebit(txin, filter);
    }
    CAmount getCredit(const CTxOut& txout, isminefilter filter) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->GetCredit(txout, filter);
    }
    bool isUnspentAddress(const std::string &qtumAddress) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        std::vector<COutput> vecOutputs;
        m_wallet->AvailableCoins(*locked_chain, vecOutputs);
        for (const COutput& out : vecOutputs)
        {
            CTxDestination address;
            const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
            bool fValidAddress = ExtractDestination(scriptPubKey, address);

            if(fValidAddress && EncodeDestination(address) == qtumAddress && out.tx->tx->vout[out.i].nValue)
            {
                return true;
            }
        }
        return false;
    }
    bool isMineAddress(const std::string &strAddress) override
    {
        CTxDestination address = DecodeDestination(strAddress);
        if(!IsValidDestination(address) || !IsMine(*m_wallet, address))
        {
            return false;
        }
        return true;
    }
    std::vector<std::string> availableAddresses(interfaces::Chain::Lock& locked_chain, bool fIncludeZeroValue)
    {
        std::vector<std::string> result;
        std::vector<COutput> vecOutputs;
        std::map<std::string, bool> mapAddress;

        if(fIncludeZeroValue)
        {
            // Get the user created addresses in from the address book and add them if they are mine
            for (const auto& item : m_wallet->mapAddressBook) {
                if(!IsMine(*m_wallet, item.first)) continue;

                std::string strAddress = EncodeDestination(item.first);
                if (mapAddress.find(strAddress) == mapAddress.end())
                {
                    mapAddress[strAddress] = true;
                    result.push_back(strAddress);
                }
            }

            // Get all coins including the 0 values
            m_wallet->AvailableCoins(locked_chain, vecOutputs, false, nullptr, 0);
        }
        else
        {
            // Get all spendable coins
            m_wallet->AvailableCoins(locked_chain, vecOutputs);
        }

        // Extract all coins addresses and add them in the list
        for (const COutput& out : vecOutputs)
        {
            CTxDestination address;
            const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
            bool fValidAddress = ExtractDestination(scriptPubKey, address);

            if (!fValidAddress || !IsMine(*m_wallet, address)) continue;

            std::string strAddress = EncodeDestination(address);
            if (mapAddress.find(strAddress) == mapAddress.end())
            {
                mapAddress[strAddress] = true;
                result.push_back(strAddress);
            }
        }

        return result;
    }
    bool tryGetAvailableAddresses(std::vector<std::string> &spendableAddresses, std::vector<std::string> &allAddresses, bool &includeZeroValue) override
    {
        auto locked_chain = m_wallet->chain().lock(true);
        if (!locked_chain) return false;
        TRY_LOCK(m_wallet->cs_wallet, locked_wallet);
        if (!locked_wallet) {
            return false;
        }

        spendableAddresses = availableAddresses(*locked_chain, false);
        allAddresses = availableAddresses(*locked_chain, true);
        includeZeroValue = ::chainActive.Height() >= Params().GetConsensus().QIP5Height;

        return true;
    }
    CoinsList listCoins() override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        CoinsList result;
        for (const auto& entry : m_wallet->ListCoins(*locked_chain)) {
            auto& group = result[entry.first];
            for (const auto& coin : entry.second) {
                group.emplace_back(COutPoint(coin.tx->GetHash(), coin.i),
                    MakeWalletTxOut(*locked_chain, *m_wallet, *coin.tx, coin.i, coin.nDepth));
            }
        }
        return result;
    }
    std::vector<WalletTxOut> getCoins(const std::vector<COutPoint>& outputs) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        std::vector<WalletTxOut> result;
        result.reserve(outputs.size());
        for (const auto& output : outputs) {
            result.emplace_back();
            auto it = m_wallet->mapWallet.find(output.hash);
            if (it != m_wallet->mapWallet.end()) {
                int depth = it->second.GetDepthInMainChain(*locked_chain);
                if (depth >= 0) {
                    result.back() = MakeWalletTxOut(*locked_chain, *m_wallet, it->second, output.n, depth);
                }
            }
        }
        return result;
    }
    CAmount getRequiredFee(unsigned int tx_bytes) override { return GetRequiredFee(*m_wallet, tx_bytes); }
    CAmount getMinimumFee(unsigned int tx_bytes,
        const CCoinControl& coin_control,
        int* returned_target,
        FeeReason* reason) override
    {
        FeeCalculation fee_calc;
        CAmount result;
        result = GetMinimumFee(*m_wallet, tx_bytes, coin_control, ::mempool, ::feeEstimator, &fee_calc);
        if (returned_target) *returned_target = fee_calc.returnedTarget;
        if (reason) *reason = fee_calc.reason;
        return result;
    }
    unsigned int getConfirmTarget() override { return m_wallet->m_confirm_target; }
    bool hdEnabled() override { return m_wallet->IsHDEnabled(); }
    bool canGetAddresses() override { return m_wallet->CanGetAddresses(); }
    bool IsWalletFlagSet(uint64_t flag) override { return m_wallet->IsWalletFlagSet(flag); }
    OutputType getDefaultAddressType() override { return m_wallet->m_default_address_type; }
    OutputType getDefaultChangeType() override { return m_wallet->m_default_change_type; }
    void remove() override
    {
        RemoveWallet(m_wallet);
    }
    bool addTokenEntry(const TokenInfo &token) override
    {
        return m_wallet->AddTokenEntry(MakeTokenInfo(token), true);
    }
    bool addTokenTxEntry(const TokenTx& tokenTx, bool fFlushOnClose) override
    {
        return m_wallet->AddTokenTxEntry(MakeTokenTx(tokenTx), fFlushOnClose);
    }
    bool existTokenEntry(const TokenInfo &token) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        uint256 hash = MakeTokenInfo(token).GetHash();
        std::map<uint256, CTokenInfo>::iterator it = m_wallet->mapToken.find(hash);

        return it != m_wallet->mapToken.end();
    }
    bool removeTokenEntry(const std::string &sHash) override
    {
        return m_wallet->RemoveTokenEntry(uint256S(sHash), true);
    }
    std::vector<TokenInfo> getInvalidTokens() override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        std::vector<TokenInfo> listInvalid;
        for(auto& info : m_wallet->mapToken)
        {
            std::string strAddress = info.second.strSenderAddress;
            CTxDestination address = DecodeDestination(strAddress);
            if(!IsMine(*m_wallet, address))
            {
                listInvalid.push_back(MakeWalletTokenInfo(info.second));
            }
        }

        return listInvalid;
    }
    TokenTx getTokenTx(const uint256& txid) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        auto mi = m_wallet->mapTokenTx.find(txid);
        if (mi != m_wallet->mapTokenTx.end()) {
            return MakeWalletTokenTx(mi->second);
        }
        return {};
    }
    std::vector<TokenTx> getTokenTxs() override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        std::vector<TokenTx> result;
        result.reserve(m_wallet->mapTokenTx.size());
        for (const auto& entry : m_wallet->mapTokenTx) {
            result.emplace_back(MakeWalletTokenTx(entry.second));
        }
        return result;
    }
    TokenInfo getToken(const uint256& id) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        auto mi = m_wallet->mapToken.find(id);
        if (mi != m_wallet->mapToken.end()) {
            return MakeWalletTokenInfo(mi->second);
        }
        return {};
    }
    std::vector<TokenInfo> getTokens() override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        std::vector<TokenInfo> result;
        result.reserve(m_wallet->mapToken.size());
        for (const auto& entry : m_wallet->mapToken) {
            result.emplace_back(MakeWalletTokenInfo(entry.second));
        }
        return result;
    }
    bool tryGetTokenTxStatus(const uint256& txid, int& block_number, bool& in_mempool, int& num_blocks) override
    {
        auto locked_chain = m_wallet->chain().lock(true);
        if (!locked_chain) {
            return false;
        }
        TRY_LOCK(m_wallet->cs_wallet, locked_wallet);
        if (!locked_wallet) {
            return false;
        }
        return TokenTxStatus(*locked_chain, *m_wallet, txid, block_number, in_mempool, num_blocks);
    }
    bool getTokenTxStatus(const uint256& txid, int& block_number, bool& in_mempool, int& num_blocks) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        return TokenTxStatus(*locked_chain, *m_wallet, txid, block_number, in_mempool, num_blocks);
    }
    bool getTokenTxDetails(const TokenTx &wtx, uint256& credit, uint256& debit, std::string& tokenSymbol, uint8_t& decimals) override
    {
        return m_wallet->GetTokenTxDetails(MakeTokenTx(wtx), credit, debit, tokenSymbol, decimals);
    }
    bool isTokenTxMine(const TokenTx &wtx) override
    {
        return m_wallet->IsTokenTxMine(MakeTokenTx(wtx));
    }
    ContractBookData getContractBook(const std::string& id) override
    {
        LOCK(m_wallet->cs_wallet);

        auto mi = m_wallet->mapContractBook.find(id);
        if (mi != m_wallet->mapContractBook.end()) {
            return MakeContractBook(id, mi->second);
        }
        return {};
    }
    std::vector<ContractBookData> getContractBooks() override
    {
        LOCK(m_wallet->cs_wallet);

        std::vector<ContractBookData> result;
        result.reserve(m_wallet->mapContractBook.size());
        for (const auto& entry : m_wallet->mapContractBook) {
            result.emplace_back(MakeContractBook(entry.first, entry.second));
        }
        return result;
    }
    bool existContractBook(const std::string& id) override
    {
        LOCK(m_wallet->cs_wallet);

        auto mi = m_wallet->mapContractBook.find(id);
        return mi != m_wallet->mapContractBook.end();
    }
    bool delContractBook(const std::string& id) override
    {
        return m_wallet->DelContractBook(id);
    }
    bool setContractBook(const std::string& id, const std::string& name, const std::string& abi) override
    {
        return m_wallet->SetContractBook(id, name, abi);
    }
    uint32_t restoreDelegations() override
    {
        RefreshDelegates(m_wallet.get(), true, false);

        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        int ret = 0;
        for (const auto& item : m_wallet->m_my_delegations) {
            DelegationDetails details = getDelegationDetails(KeyIdToString(item.first));
            if(!details.w_entry_exist && details.c_entry_exist)
            {
                DelegationInfo info = details.toInfo(false);
                info.staker_name = info.staker_address;
                if(addDelegationEntry(info))
                    ret++;
            }
        }

        return ret;
    }
    bool addDelegationEntry(const DelegationInfo &delegation) override
    {
        return m_wallet->AddDelegationEntry(MakeDelegationInfo(delegation), true);
    }
    bool existDelegationEntry(const DelegationInfo &delegation) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        uint256 hash = MakeDelegationInfo(delegation).GetHash();
        std::map<uint256, CDelegationInfo>::iterator it = m_wallet->mapDelegation.find(hash);

        return it != m_wallet->mapDelegation.end();
    }
    DelegationInfo getDelegation(const uint256& id) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        auto mi = m_wallet->mapDelegation.find(id);
        if (mi != m_wallet->mapDelegation.end()) {
            return MakeWalletDelegationInfo(mi->second);
        }
        return {};
    }
    DelegationInfo getDelegationContract(const std::string &sHash, bool& validated, bool& contractRet) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        uint256 id;
        id.SetHex(sHash);
        auto mi = m_wallet->mapDelegation.find(id);
        if (mi != m_wallet->mapDelegation.end()) {
            DelegationInfo info = MakeWalletDelegationInfo(mi->second);
            Delegation delegation;
            CTxDestination dest = DecodeDestination(info.delegate_address);
            const CKeyID *keyID = boost::get<CKeyID>(&dest);
            if(keyID)
            {
                uint160 address(*keyID);
                contractRet = m_qtumDelegation.ExistDelegationContract() ? m_qtumDelegation.GetDelegation(address, delegation) : false;
                if(contractRet)
                {
                    validated = m_qtumDelegation.VerifyDelegation(address, delegation);
                    info.staker_address = EncodeDestination(CKeyID(delegation.staker));
                    info.fee = delegation.fee;
                    info.block_number = delegation.blockHeight;
                }
                return info;
            }
        }
        return {};
    }
    DelegationDetails getDelegationDetails(const std::string &sAddress) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        DelegationDetails details;

        // Get wallet delegation details
        for(auto mi : m_wallet->mapDelegation)
        {
            if(KeyIdToString(mi.second.delegateAddress) == sAddress)
            {
                details.w_entry_exist = true;
                details.w_delegate_address = KeyIdToString(mi.second.delegateAddress);
                details.w_staker_address = KeyIdToString(mi.second.stakerAddress);
                details.w_staker_name = mi.second.strStakerName;
                details.w_fee = mi.second.nFee;
                details.w_time = mi.second.nCreateTime;
                details.w_block_number = mi.second.blockNumber;
                details.w_hash = mi.first;
                details.w_create_tx_hash = mi.second.createTxHash;
                details.w_remove_tx_hash = mi.second.removeTxHash;
                break;
            }
        }

        // Get wallet create tx details
        const CWalletTx* wtx = m_wallet->GetWalletTx(details.w_create_tx_hash);
        if(wtx)
        {
            details.w_create_exist = true;
            details.w_create_in_main_chain = wtx->IsInMainChain(*locked_chain);
            details.w_create_in_mempool = wtx->InMempool();
            details.w_create_abandoned = wtx->isAbandoned();
        }

        // Get wallet remove tx details
        wtx = m_wallet->GetWalletTx(details.w_remove_tx_hash);
        if(wtx)
        {
            details.w_remove_exist = true;
            details.w_remove_in_main_chain = wtx->IsInMainChain(*locked_chain);
            details.w_remove_in_mempool = wtx->InMempool();
            details.w_remove_abandoned = wtx->isAbandoned();
        }

        // Delegation contract details
        Delegation delegation;
        CTxDestination dest = DecodeDestination(sAddress);
        const CKeyID *keyID = boost::get<CKeyID>(&dest);
        if(keyID)
        {
            uint160 address(*keyID);
            details.c_contract_return = m_qtumDelegation.ExistDelegationContract() ? m_qtumDelegation.GetDelegation(address, delegation) : false;
            if(details.c_contract_return)
            {
                details.c_entry_exist = m_qtumDelegation.VerifyDelegation(address, delegation);
                details.c_delegate_address = sAddress;
                details.c_staker_address = EncodeDestination(CKeyID(delegation.staker));
                details.c_fee = delegation.fee;
                details.c_block_number = delegation.blockHeight;
            }
        }

        return details;
    }
    std::vector<DelegationInfo> getDelegations() override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        std::vector<DelegationInfo> result;
        result.reserve(m_wallet->mapDelegation.size());
        for (const auto& entry : m_wallet->mapDelegation) {
            result.emplace_back(MakeWalletDelegationInfo(entry.second));
        }
        return result;
    }
    bool removeDelegationEntry(const std::string &sHash) override
    {
        return m_wallet->RemoveDelegationEntry(uint256S(sHash), true);
    }
    bool setDelegationRemoved(const std::string &sHash, const std::string &sTxid)
    {
        bool found = false;
        DelegationInfo info;
        {
            auto locked_chain = m_wallet->chain().lock();
            LOCK(m_wallet->cs_wallet);

            uint256 id;
            id.SetHex(sHash);

            uint256 txid;
            txid.SetHex(sTxid);

            auto mi = m_wallet->mapDelegation.find(id);
            if (mi != m_wallet->mapDelegation.end()) {
                info = MakeWalletDelegationInfo(mi->second);
                info.remove_tx_hash = txid;
                found = true;
            }
        }

        return found ? addDelegationEntry(info) : 0;
    }
    uint32_t restoreSuperStakers() override
    {
        RefreshDelegates(m_wallet.get(), false, true);
        
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        std::map<uint160, bool> stakerAddressExist;
        for (const auto& item : m_wallet->m_delegations_staker) {
            uint160 staker = item.second.staker;
            if(!stakerAddressExist[staker])
                stakerAddressExist[staker] = true;
        }

        int ret = 0;
        for (const auto& item : stakerAddressExist) {
            std::string staker_address = KeyIdToString(item.first);
            if(!existSuperStaker(staker_address))
            {
                SuperStakerInfo info;
                info.staker_name = staker_address;
                info.staker_address = staker_address;
                if(addSuperStakerEntry(info))
                    ret++;
            }
        }

        return ret;
    }
    bool existSuperStaker(const std::string &sAddress) override
    {
        LOCK(m_wallet->cs_wallet);
        uint160 address = StringToKeyId(sAddress);
        if(address.IsNull())
            return false;

        for (const auto& entry : m_wallet->mapSuperStaker) {
            if(entry.second.stakerAddress == address)
                return true;
        }

        return false;
    }
    SuperStakerInfo getSuperStaker(const uint256& id) override
    {
        LOCK(m_wallet->cs_wallet);

        auto mi = m_wallet->mapSuperStaker.find(id);
        if (mi != m_wallet->mapSuperStaker.end()) {
            return MakeWalletSuperStakerInfo(mi->second);
        }
        return {};
    }
    SuperStakerInfo getSuperStakerRecommendedConfig() override
    {
        LOCK(m_wallet->cs_wallet);

        // Set recommended config
        SuperStakerInfo config;
        config.custom_config = false;
        config.min_fee = m_wallet->m_staking_min_fee;
        config.min_delegate_utxo = m_wallet->m_staking_min_utxo_value;
        config.delegate_address_type = SuperStakerAddressList::AcceptAll;

        // Get white list
        std::vector<std::string> whiteList;
        for (const std::string& strAddress : gArgs.GetArgs("-stakingwhitelist"))
        {
            if(!StringToKeyId(strAddress).IsNull())
            {
                if(std::find(whiteList.begin(), whiteList.end(), strAddress) == whiteList.end())
                    whiteList.push_back(strAddress);
            }
        }

        // Get black list
        std::vector<std::string> blackList;
        for (const std::string& strAddress : gArgs.GetArgs("-stakingblacklist"))
        {
            if(!StringToKeyId(strAddress).IsNull())
            {
                if(std::find(blackList.begin(), blackList.end(), strAddress) == blackList.end())
                    blackList.push_back(strAddress);
            }
        }

        // Set the address list
        if(!whiteList.empty())
        {
            config.delegate_address_type =  SuperStakerAddressList::WhiteList;
            config.delegate_address_list = whiteList;
        }
        else if(!blackList.empty())
        {
            config.delegate_address_type = SuperStakerAddressList::BlackList;
            config.delegate_address_list = blackList;
        }

        return config;
    }
    std::vector<SuperStakerInfo> getSuperStakers() override
    {
        LOCK(m_wallet->cs_wallet);

        std::vector<SuperStakerInfo> result;
        result.reserve(m_wallet->mapSuperStaker.size());
        for (const auto& entry : m_wallet->mapSuperStaker) {
            result.emplace_back(MakeWalletSuperStakerInfo(entry.second));
        }
        return result;
    }
    bool addSuperStakerEntry(const SuperStakerInfo &superStaker) override
    {
        return m_wallet->AddSuperStakerEntry(MakeSuperStakerInfo(superStaker), true);
    }
    bool removeSuperStakerEntry(const std::string &sHash) override
    {
        return m_wallet->RemoveSuperStakerEntry(uint256S(sHash), true);
    }
    bool tryGetStakeWeight(uint64_t& nWeight) override
    {
        auto locked_chain = m_wallet->chain().lock(true);
        if (!locked_chain) {
            return false;
        }
        TRY_LOCK(m_wallet->cs_wallet, locked_wallet);
        if (!locked_wallet) {
            return false;
        }

        nWeight = m_wallet->GetStakeWeight(*locked_chain);
        return true;
    }
    uint64_t getStakeWeight() override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);
        return m_wallet->GetStakeWeight(*locked_chain);
    }
    int64_t getLastCoinStakeSearchInterval() override 
    { 
        return m_wallet->m_last_coin_stake_search_interval;
    }
    bool getWalletUnlockStakingOnly() override
    {
        return m_wallet->m_wallet_unlock_staking_only;
    }
    void setWalletUnlockStakingOnly(bool unlock) override
    {
        m_wallet->m_wallet_unlock_staking_only = unlock;
    }
    bool cleanTokenTxEntries() override
    {
        return m_wallet->CleanTokenTxEntries();
    }
    void setEnabledStaking(bool enabled) override
    {
        m_wallet->m_enabled_staking = enabled;
    }
    bool getEnabledStaking() override
    {
        return m_wallet->m_enabled_staking;
    }
    bool getEnabledSuperStaking() override
    {
        bool fSuperStake = gArgs.GetBoolArg("-superstaking", DEFAULT_SUPER_STAKE);
        return fSuperStake;
    }
    DelegationStakerInfo getDelegationStaker(const uint160& id) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        auto mi = m_wallet->m_delegations_staker.find(id);
        if (mi != m_wallet->m_delegations_staker.end()) {
            return MakeWalletDelegationStakerInfo(*locked_chain, *m_wallet, mi->first, mi->second);
        }
        return {};
    }
    std::vector<DelegationStakerInfo> getDelegationsStakers() override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        std::vector<DelegationStakerInfo> result;
        result.reserve(m_wallet->m_delegations_staker.size());
        for (const auto& entry : m_wallet->m_delegations_staker) {
            result.emplace_back(MakeWalletDelegationStakerInfo(*locked_chain, *m_wallet, entry.first, entry.second));
        }
        return result;
    }
    uint64_t getSuperStakerWeight(const uint256& id) override
    {
        LOCK(m_wallet->cs_wallet);
        SuperStakerInfo info = getSuperStaker(id);
        CTxDestination dest = DecodeDestination(info.staker_address);
        const CKeyID *keyID = boost::get<CKeyID>(&dest);
        if(keyID)
        {
            uint160 address(*keyID);
            return m_wallet->GetSuperStakerWeight(address);
        }

        return 0;
    }
    bool isSuperStakerStaking(const uint256& id, CAmount& delegationsWeight) override
    {
        uint64_t lastCoinStakeSearchInterval = getEnabledStaking() ? getLastCoinStakeSearchInterval() : 0;
        delegationsWeight = getSuperStakerWeight(id);
        return lastCoinStakeSearchInterval && delegationsWeight;
    }
    bool getStakerAddressBalance(const std::string& staker, CAmount& balance, CAmount& stake, CAmount& weight) override
    {
        auto locked_chain = m_wallet->chain().lock();
        LOCK(m_wallet->cs_wallet);

        CTxDestination dest = DecodeDestination(staker);
        const CKeyID *keyID = boost::get<CKeyID>(&dest);
        if(keyID)
        {
            m_wallet->GetStakerAddressBalance(*locked_chain, *keyID, balance, stake, weight);
        }

        return keyID != 0;
    }
    std::unique_ptr<Handler> handleUnload(UnloadFn fn) override
    {
        return MakeHandler(m_wallet->NotifyUnload.connect(fn));
    }
    std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) override
    {
        return MakeHandler(m_wallet->ShowProgress.connect(fn));
    }
    std::unique_ptr<Handler> handleStatusChanged(StatusChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyStatusChanged.connect([fn](CCryptoKeyStore*) { fn(); }));
    }
    std::unique_ptr<Handler> handleAddressBookChanged(AddressBookChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyAddressBookChanged.connect(
            [fn](CWallet*, const CTxDestination& address, const std::string& label, bool is_mine,
                const std::string& purpose, ChangeType status) { fn(address, label, is_mine, purpose, status); }));
    }
    std::unique_ptr<Handler> handleTransactionChanged(TransactionChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyTransactionChanged.connect(
            [fn](CWallet*, const uint256& txid, ChangeType status) { fn(txid, status); }));
    }
    std::unique_ptr<Handler> handleTokenTransactionChanged(TokenTransactionChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyTokenTransactionChanged.connect(
            [fn](CWallet*, const uint256& id, ChangeType status) { fn(id, status); }));
    }
    std::unique_ptr<Handler> handleTokenChanged(TokenChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyTokenChanged.connect(
            [fn](CWallet*, const uint256& id, ChangeType status) { fn(id, status); }));
    }
    std::unique_ptr<Handler> handleWatchOnlyChanged(WatchOnlyChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyWatchonlyChanged.connect(fn));
    }
    std::unique_ptr<Handler> handleCanGetAddressesChanged(CanGetAddressesChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyCanGetAddressesChanged.connect(fn));
    }
    std::unique_ptr<Handler> handleContractBookChanged(ContractBookChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyContractBookChanged.connect(
            [fn](CWallet*, const std::string& address, const std::string& label,
                const std::string& abi, ChangeType status) { fn(address, label, abi, status); }));
    }
    std::unique_ptr<Handler> handleDelegationChanged(DelegationChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyDelegationChanged.connect(
            [fn](CWallet*, const uint256& id, ChangeType status) { fn(id, status); }));
    }
    std::unique_ptr<Handler> handleSuperStakerChanged(SuperStakerChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifySuperStakerChanged.connect(
            [fn](CWallet*, const uint256& id, ChangeType status) { fn(id, status); }));
    }
    std::unique_ptr<Handler> handleDelegationsStakerChanged(DelegationsStakerChangedFn fn) override
    {
        return MakeHandler(m_wallet->NotifyDelegationsStakerChanged.connect(
            [fn](CWallet*, const uint160& id, ChangeType status) { fn(id, status); }));
    }

    std::shared_ptr<CWallet> m_wallet;
    QtumDelegation m_qtumDelegation;
};

class WalletClientImpl : public ChainClient
{
public:
    WalletClientImpl(Chain& chain, std::vector<std::string> wallet_filenames)
        : m_chain(chain), m_wallet_filenames(std::move(wallet_filenames))
    {
    }
    void registerRpcs() override { return RegisterWalletRPCCommands(::tableRPC); }
    bool verify() override { return VerifyWallets(m_chain, m_wallet_filenames); }
    bool load() override { return LoadWallets(m_chain, m_wallet_filenames); }
    void start(CScheduler& scheduler) override { return StartWallets(scheduler); }
    void flush() override { return FlushWallets(); }
    void stop() override { return StopWallets(); }
    ~WalletClientImpl() override { UnloadWallets(); }

    Chain& m_chain;
    std::vector<std::string> m_wallet_filenames;
};

} // namespace

std::unique_ptr<Wallet> MakeWallet(const std::shared_ptr<CWallet>& wallet) { return wallet ? MakeUnique<WalletImpl>(wallet) : nullptr; }

std::unique_ptr<ChainClient> MakeWalletClient(Chain& chain, std::vector<std::string> wallet_filenames)
{
    return MakeUnique<WalletClientImpl>(chain, std::move(wallet_filenames));
}

} // namespace interfaces
