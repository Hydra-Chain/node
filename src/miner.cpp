// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <miner.h>

#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <hash.h>
#include <net.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pow.h>
#include <pos.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <timedata.h>
#include <util/convert.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <validationinterface.h>
#include <key_io.h>
#ifdef ENABLE_WALLET
#include <wallet/wallet.h>
#endif

#include <algorithm>
#include <queue>
#include <utility>
#include <stdint.h>

#include "locktrip/economy.h"
#include "locktrip/dgp.h"
#include "locktrip/lydra.h"

//////////////////////////////////////////////////////////////////////////////
//
// HydraMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest fee rate of a transaction combined with all
// its ancestors.

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockWeight = 0;
unsigned int nMaxStakeLookahead = MAX_STAKE_LOOKAHEAD;
unsigned int nBytecodeTimeBuffer = BYTECODE_TIME_BUFFER;
unsigned int nStakeTimeBuffer = STAKE_TIME_BUFFER;
unsigned int nMinerSleep = STAKER_POLLING_PERIOD;
unsigned int nMinerWaitWalidBlock = STAKER_WAIT_FOR_WALID_BLOCK;
unsigned int nMinerWaitBestBlockHeader = STAKER_WAIT_FOR_BEST_BLOCK_HEADER;

void updateMinerParams(int nHeight, const Consensus::Params& consensusParams, bool minDifficulty)
{
    static unsigned int timeDownscale = 1;
    static unsigned int timeDefault = 1;
    unsigned int timeDownscaleTmp = consensusParams.TimestampDownscaleFactor(nHeight);
    if(timeDownscale != timeDownscaleTmp)
    {
        timeDownscale = timeDownscaleTmp;
        unsigned int targetSpacing =  consensusParams.TargetSpacing(nHeight);
        nMaxStakeLookahead = std::max(MAX_STAKE_LOOKAHEAD / timeDownscale, timeDefault);
        nMaxStakeLookahead = std::min(nMaxStakeLookahead, targetSpacing);
        nBytecodeTimeBuffer = std::max(BYTECODE_TIME_BUFFER / timeDownscale, timeDefault);
        nStakeTimeBuffer = std::max(STAKE_TIME_BUFFER / timeDownscale, timeDefault);
        nMinerSleep = std::max(STAKER_POLLING_PERIOD / timeDownscale, timeDefault);
        nMinerWaitWalidBlock = std::max(STAKER_WAIT_FOR_WALID_BLOCK / timeDownscale, timeDefault);
    }

    // Sleep for 20 seconds when mining with minimum difficulty to avoid creating blocks every 4 seconds
    if(minDifficulty && nMinerSleep != STAKER_POLLING_PERIOD_MIN_DIFFICULTY)
    {
        nMinerSleep = STAKER_POLLING_PERIOD_MIN_DIFFICULTY;
    }
}

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    if (nOldTime < nNewTime)
        pblock->nTime = nNewTime;

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks)
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams,pblock->IsProofOfStake());

    return nNewTime - nOldTime;
}

BlockAssembler::Options::Options() {
    blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    nBlockMaxWeight = DEFAULT_BLOCK_MAX_WEIGHT;
}

BlockAssembler::BlockAssembler(const CChainParams& params, const Options& options) : chainparams(params)
{
    blockMinFeeRate = options.blockMinFeeRate;
    // Limit weight to between 4K and dgpMaxBlockWeight-4K for sanity:
    nBlockMaxWeight = std::max<size_t>(4000, std::min<size_t>(dgpMaxBlockWeight - 4000, options.nBlockMaxWeight));
}

static BlockAssembler::Options DefaultOptions()
{
    // Block resource limits
    // If -blockmaxweight is not given, limit to DEFAULT_BLOCK_MAX_WEIGHT
    BlockAssembler::Options options;
    options.nBlockMaxWeight = gArgs.GetArg("-blockmaxweight", DEFAULT_BLOCK_MAX_WEIGHT);
    CAmount n = 0;
    if (gArgs.IsArgSet("-blockmintxfee") && ParseMoney(gArgs.GetArg("-blockmintxfee", ""), n)) {
        options.blockMinFeeRate = CFeeRate(n);
    } else {
        options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    }
    return options;
}

BlockAssembler::BlockAssembler(const CChainParams& params) : BlockAssembler(params, DefaultOptions()) {}

#ifdef ENABLE_WALLET
BlockAssembler::BlockAssembler(const CChainParams& params, CWallet *_pwallet)
    : BlockAssembler(params)
{
    pwallet = _pwallet;
}
#endif

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockWeight = 4000;
    nBlockSigOpsCost = 400;
    fIncludeWitness = false;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
}

void BlockAssembler::BurnMinerFees(){
    Dgp dgp;
    uint64_t coinBurnPercentage;
    dgp.getDgpParam(BURN_RATE, coinBurnPercentage);
    if(coinBurnPercentage > MAX_BURN_RATE_PERCENTAGE || coinBurnPercentage < MIN_BURN_RATE_PERCENTAGE ){
        coinBurnPercentage = DEFAULT_BURN_RATE_PERCENTAGE;
    }

    int refundtx=0; //0 for coinbase in PoW
    if(pblock->IsProofOfStake()){
        refundtx=1; //1 for coinstake in PoS
    }

    auto coinAmountThatSouldBeBurned =  ((nFees - bceResult.refundSender) / 100) * coinBurnPercentage;
    originalRewardTx.vout[refundtx].nValue -= coinAmountThatSouldBeBurned;
    nFees -= coinAmountThatSouldBeBurned;
}

void BlockAssembler::AddDividentsToCoinstakeTransaction(){
    int refundtx=0; //0 for coinbase in PoW
    if(pblock->IsProofOfStake()){
        refundtx=1; //1 for coinstake in PoS
    }

    std::map<dev::Address, CAmount> dividendsPerAddress;
    for (const auto &pair : bceResult.dividendByContract) {
        if (dividendsPerAddress.count(pair.first)) {
            dividendsPerAddress[pair.first] += pair.second;
        } else {
            dividendsPerAddress.insert(std::pair<dev::Address, CAmount>(pair.first, pair.second));
        }
    }

    Economy e;
    dev::Address contractOwner;
    std::vector<CTxOut> dividends;
    for (const auto& pair : dividendsPerAddress) {
        e.getContractOwner(pair.first, contractOwner);
        //if the contract doesn't have owner we won't pay dividents.Contracts without owners should be dgb contracts and economy contract.
        if(contractOwner != dev::Address("0"))
        {
            originalRewardTx.vout[refundtx].nValue -= pair.second; // Remove the dividents from miner fee and reward.
            nFees -= pair.second;

            CScript script(CScript() << OP_DUP << OP_HASH160 << contractOwner.asBytes() << OP_EQUALVERIFY << OP_CHECKSIG);
            CTxOut dividend(pair.second, script);
            dividends.push_back(dividend);
        }
    }

    originalRewardTx.vout.insert(std::end(originalRewardTx.vout), std::begin(dividends), std::end(dividends));
}

void BlockAssembler::CalculateRewardWithoutDividents() {
    int refundtx=0; //0 for coinbase in PoW
    if(pblock->IsProofOfStake()){
        refundtx=1; //1 for coinstake in PoS
    }
    originalRewardTx.vout[refundtx].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
    originalRewardTx.vout[refundtx].nValue -= bceResult.refundSender;
}

void BlockAssembler::AddRefundTransactions(){
    //note, this will need changed for MPoS
    int i=originalRewardTx.vout.size();
    originalRewardTx.vout.resize(originalRewardTx.vout.size() + bceResult.refundOutputs.size());
    for(CTxOut& vout : bceResult.refundOutputs){
        originalRewardTx.vout[i]=vout;
        i++;
    }
}

void BlockAssembler::AddTransactionForFinishingVote(){
    Dgp d;
    CScript scriptPubKey;
    d.finishVote(scriptPubKey);

    originalRewardTx.vout.resize(originalRewardTx.vout.size() + 1);
    int lastElementIndex = originalRewardTx.vout.size() - 1;
    originalRewardTx.vout[lastElementIndex].scriptPubKey = scriptPubKey;
    originalRewardTx.vout[lastElementIndex].nValue = 0;
}

void BlockAssembler::AddTransactionForSavingContractOwners(){
    Economy e;
    CScript scriptPubKey;
    e.getCScriptForAddContract(bceResult.contractAddresses, bceResult.contractOwners, scriptPubKey);

    originalRewardTx.vout.resize(originalRewardTx.vout.size() + 1);
    int lastElementIndex = originalRewardTx.vout.size() - 1;
    originalRewardTx.vout[lastElementIndex].scriptPubKey = scriptPubKey;
    originalRewardTx.vout[lastElementIndex].nValue = 0;
}

bool BlockAssembler::ExecuteCoinstakeContractCalls(CWallet& wallet, int64_t* pTotalFees, int32_t txProofTime,
        std::set<std::pair<const CWalletTx*,unsigned int> > setCoins, std::vector<COutPoint>& setSelectedCoins, std::vector<COutPoint>& setDelegateCoins){
    CKey key;
    CMutableTransaction txCoinStake(*pblock->vtx[1]);
    uint32_t nTimeBlock = txProofTime;
    nTimeBlock &= ~chainparams.GetConsensus().StakeTimestampMask(nHeight);
    auto locked_chain = wallet.chain().lock();
    std::vector<unsigned char> vchPoD;
    COutPoint headerPrevout;

    wallet.CreateCoinStake(*locked_chain, wallet, pblock->nBits, *pTotalFees, nTimeBlock, txCoinStake, key, setCoins, setSelectedCoins, setDelegateCoins, true, vchPoD, headerPrevout);
    auto tx = MakeTransactionRef(txCoinStake);
    QtumTxConverter convert(*(tx.get()), NULL, &pblock->vtx);

    ExtractQtumTX resultConverter;
    if (!convert.extractionQtumTransactions(resultConverter)) {
        LogPrintf("ERROR: %s - Error converting txs to qtum txs\n", __func__);
        return false;
    }

    std::vector<QtumTransaction> qtumTransactions = resultConverter.first;
    dev::h256 oldHashStateRoot(globalState->rootHash());
    dev::h256 oldHashUTXORoot(globalState->rootHashUTXO());

    ByteCodeExec exec(*pblock, qtumTransactions, INT64_MAX, chainActive.Tip());
    if (!exec.performByteCode()) {
        LogPrintf("ERROR: %s - Error performing byte code\n", __func__);
        //error, don't add contract
        globalState->setRoot(oldHashStateRoot);
        globalState->setRootUTXO(oldHashUTXORoot);
        return false;
    }

    ByteCodeExecResult testExecResult;
    if(!exec.processingResults(testExecResult)){
        LogPrintf("ERROR: %s - Error processing results\n", __func__);
        globalState->setRoot(oldHashStateRoot);
        globalState->setRootUTXO(oldHashUTXORoot);
        return false;
    }

    bool hasExceptions = std::any_of(
            std::begin(testExecResult.execExceptions),
            std::end(testExecResult.execExceptions),
            [](dev::eth::TransactionException ex) {
                return ex != dev::eth::TransactionException::None;
            });

    if(hasExceptions){
        LogPrintf("ERROR: %s - exception raised during contract executions\n", __func__);
        globalState->setRoot(oldHashStateRoot);
        globalState->setRootUTXO(oldHashUTXORoot);
        return false;
    }

    return  true;
}

void BlockAssembler::ReplaceRewardTransaction() {
    int refundtx=0; //0 for coinbase in PoW
    if(pblock->IsProofOfStake()){
        refundtx=1; //1 for coinstake in PoS
    }
    pblock->vtx[refundtx] = MakeTransactionRef(std::move(originalRewardTx));
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn, bool fMineWitnessTx,
        bool fProofOfStake, int64_t* pTotalFees, int32_t txProofTime, int32_t nTimeLimit, CWallet* wallet,
        std::set<std::pair<const CWalletTx*,unsigned int> > setCoins, std::vector<COutPoint> setSelectedCoins, std::vector<COutPoint> setDelegateCoins)
{
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if(!pblocktemplate.get())
        return nullptr;
    pblock = &pblocktemplate->block; // pointer for convenience

    this->nTimeLimit = nTimeLimit;

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    // Add dummy coinstake tx as second transaction
    if(fProofOfStake)
        pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

#ifdef ENABLE_WALLET
    if(pwallet && pwallet->IsStakeClosing())
        return nullptr;
#endif
    LOCK2(cs_main, mempool.cs);
    CBlockIndex* pindexPrev = chainActive.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    pblock->nVersion = ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand())
        pblock->nVersion = gArgs.GetArg("-blockversion", pblock->nVersion);

    if(txProofTime == 0) {
        txProofTime = GetAdjustedTime();
    }
    if(fProofOfStake)
        txProofTime &= ~chainparams.GetConsensus().StakeTimestampMask(nHeight);
    pblock->nTime = txProofTime;
    if (!fProofOfStake)
        UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus(),fProofOfStake);
    const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

    nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                       ? nMedianTimePast
                       : pblock->GetBlockTime();

    // Decide whether to include witness transactions
    // This is only needed in case the witness softfork activation is reverted
    // (which would require a very deep reorganization).
    // Note that the mempool would accept transactions with witness data before
    // IsWitnessEnabled, but we would only ever mine blocks after IsWitnessEnabled
    // unless there is a massive block reorganization with the witness softfork
    // not activated.
    // TODO: replace this with a call to main to assess validity of a mempool
    // transaction (which in most cases can be a no-op).
    fIncludeWitness = IsWitnessEnabled(pindexPrev, chainparams.GetConsensus()) && fMineWitnessTx;

    int64_t nTime1 = GetTimeMicros();

    nLastBlockTx = nBlockTx;
    nLastBlockWeight = nBlockWeight;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vout.resize(1);
    if (fProofOfStake)
    {
        // Make the coinbase tx empty in case of proof of stake
        coinbaseTx.vout[0].SetEmpty();
    }
    else
    {
        coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
        coinbaseTx.vout[0].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
    }
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    originalRewardTx = coinbaseTx;
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));

    // Create coinstake transaction.
    if(fProofOfStake)
    {
        CMutableTransaction coinstakeTx;
        coinstakeTx.vout.resize(2);
        coinstakeTx.vout[0].SetEmpty();
        coinstakeTx.vout[1].scriptPubKey = scriptPubKeyIn;
        originalRewardTx = coinstakeTx;
        pblock->vtx[1] = MakeTransactionRef(std::move(coinstakeTx));

        //this just makes CBlock::IsProofOfStake to return true
        //real prevoutstake info is filled in later in SignBlock
        pblock->prevoutStake.n=0;

    }

    //////////////////////////////////////////////////////// qtum
    QtumDGP qtumDGP(globalState.get(), fGettingValuesDGP);
    globalSealEngine->setQtumSchedule(qtumDGP.getGasSchedule(nHeight, chainparams.GetConsensus(), chainparams.NetworkIDString()));
    uint32_t blockSizeDGP = qtumDGP.getBlockSize(nHeight);
    minGasPrice = qtumDGP.getMinGasPrice(nHeight);
    if(gArgs.IsArgSet("-staker-min-tx-gas-price")) {
        CAmount stakerMinGasPrice;
        if(ParseMoney(gArgs.GetArg("-staker-min-tx-gas-price", ""), stakerMinGasPrice)) {
            minGasPrice = std::max(minGasPrice, (uint64_t)stakerMinGasPrice);
        }
    }
    hardBlockGasLimit = qtumDGP.getBlockGasLimit(nHeight);
    softBlockGasLimit = gArgs.GetArg("-staker-soft-block-gas-limit", hardBlockGasLimit);
    softBlockGasLimit = std::min(softBlockGasLimit, hardBlockGasLimit);
    txGasLimit = gArgs.GetArg("-staker-max-tx-gas-limit", softBlockGasLimit);

    nBlockMaxWeight = blockSizeDGP ? blockSizeDGP * WITNESS_SCALE_FACTOR : nBlockMaxWeight;
    
    dev::h256 oldHashStateRoot(globalState->rootHash());
    dev::h256 oldHashUTXORoot(globalState->rootHashUTXO());
    ////////////////////////////////////////////////// deploy offline staking contract
    if(nHeight == chainparams.GetConsensus().nOfflineStakeHeight ||
        nHeight == chainparams.GetConsensus().nDelegationsGasFixHeight){
        globalState->deployDelegationsContract(nHeight);
    }
    /////////////////////////////////////////////////
    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;

    Dgp dgp;
    bool voteInProgress;
    dgp.hasVoteInProgress(voteInProgress);
    if(voteInProgress) {
        uint64_t expiration;
        dgp.getVoteBlockExpiration(expiration);
        if (nHeight < expiration) {
            // If there will be a DGP vote finishing, don't add transactions
            addPackageTxs(nPackagesSelected, nDescendantsUpdated, minGasPrice);
        }
    } else {
        addPackageTxs(nPackagesSelected, nDescendantsUpdated, minGasPrice);
    }

    CalculateRewardWithoutDividents();
    AddRefundTransactions();
    bool hasCoinstakeCall = false;
    if(fProofOfStake) {
        if(bceResult.contractAddresses.size() != bceResult.contractOwners.size())
        {
            globalState->setRoot(oldHashStateRoot);
            globalState->setRootUTXO(oldHashUTXORoot);
            LogPrintf("ERROR: %s - Contract addresses and contract owners are with different count in the block\n", __func__);
            return nullptr;
        }

        AddDividentsToCoinstakeTransaction();
        if(bceResult.contractAddresses.size() != 0) {
            AddTransactionForSavingContractOwners();
            hasCoinstakeCall = true;
        }

        Dgp dgp;
        bool voteInProgress;
        dgp.hasVoteInProgress(voteInProgress);
        if(voteInProgress) {
            uint64_t expiration;
            dgp.getVoteBlockExpiration(expiration);

            if (nHeight >= expiration) {
                AddTransactionForFinishingVote();
                hasCoinstakeCall = true;
            }
        }

        BurnMinerFees();
    }

    ReplaceRewardTransaction();

    pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, chainparams.GetConsensus(), fProofOfStake);
    pblocktemplate->vTxFees[0] = -nFees;

    LogPrintf("CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d\n", GetBlockWeight(*pblock), nBlockTx, nFees, nBlockSigOpsCost);

    // The total fee is the Fees minus the Refund
    if (pTotalFees)
        *pTotalFees = nFees - bceResult.refundSender;

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    pblock->nNonce         = 0;
    pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);

    if(fProofOfStake && hasCoinstakeCall) {
        if(!ExecuteCoinstakeContractCalls(*wallet, pTotalFees, txProofTime, setCoins, setSelectedCoins, setDelegateCoins)) {
            globalState->setRoot(oldHashStateRoot);
            globalState->setRootUTXO(oldHashUTXORoot);
            LogPrintf("ERROR: %s - Execution of coinstake contract calls failed!\n");
            return nullptr;
        }
    }

    pblock->hashStateRoot = uint256(h256Touint(dev::h256(globalState->rootHash())));
    pblock->hashUTXORoot = uint256(h256Touint(dev::h256(globalState->rootHashUTXO())));
    globalState->setRoot(oldHashStateRoot);
    globalState->setRootUTXO(oldHashUTXORoot);

    CValidationState state;
    if (!fProofOfStake && !TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
    }
    int64_t nTime2 = GetTimeMicros();

    LogPrint(BCLog::BENCH, "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms)\n", 0.001 * (nTime1 - nTimeStart), nPackagesSelected, nDescendantsUpdated, 0.001 * (nTime2 - nTime1), 0.001 * (nTime2 - nTimeStart));

    return std::move(pblocktemplate);
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateEmptyBlock(const CScript& scriptPubKeyIn, bool fMineWitnessTx, bool fProofOfStake, int64_t* pTotalFees, int32_t nTime)
{
    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if(!pblocktemplate.get())
        return nullptr;
    pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    // Add dummy coinstake tx as second transaction
    if(fProofOfStake)
        pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

#ifdef ENABLE_WALLET
    if(pwallet && pwallet->IsStakeClosing())
        return nullptr;
#endif
    LOCK2(cs_main, mempool.cs);
    CBlockIndex* pindexPrev = chainActive.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    pblock->nVersion = ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand())
        pblock->nVersion = gArgs.GetArg("-blockversion", pblock->nVersion);

    uint32_t txProofTime = nTime == 0 ? GetAdjustedTime() : nTime;
    if(fProofOfStake)
        txProofTime &= ~chainparams.GetConsensus().StakeTimestampMask(nHeight);
    pblock->nTime = txProofTime;
    const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

    nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                       ? nMedianTimePast
                       : pblock->GetBlockTime();

    // Decide whether to include witness transactions
    // This is only needed in case the witness softfork activation is reverted
    // (which would require a very deep reorganization) or when
    // -promiscuousmempoolflags is used.
    // TODO: replace this with a call to main to assess validity of a mempool
    // transaction (which in most cases can be a no-op).
    fIncludeWitness = IsWitnessEnabled(pindexPrev, chainparams.GetConsensus()) && fMineWitnessTx;

    nLastBlockTx = nBlockTx;
    nLastBlockWeight = nBlockWeight;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vout.resize(1);
    if (fProofOfStake)
    {
        // Make the coinbase tx empty in case of proof of stake
        coinbaseTx.vout[0].SetEmpty();
    }
    else
    {
        coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
        coinbaseTx.vout[0].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
    }
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    originalRewardTx = coinbaseTx;
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));

    // Create coinstake transaction.
    if(fProofOfStake)
    {
        CMutableTransaction coinstakeTx;
        coinstakeTx.vout.resize(2);
        coinstakeTx.vout[0].SetEmpty();
        coinstakeTx.vout[1].scriptPubKey = scriptPubKeyIn;
        originalRewardTx = coinstakeTx;
        pblock->vtx[1] = MakeTransactionRef(std::move(coinstakeTx));

        //this just makes CBlock::IsProofOfStake to return true
        //real prevoutstake info is filled in later in SignBlock
        pblock->prevoutStake.n=0;
    }

    //////////////////////////////////////////////////////// qtum
    //state shouldn't change here for an empty block, but if it's not valid it'll fail in CheckBlock later
    pblock->hashStateRoot = uint256(h256Touint(dev::h256(globalState->rootHash())));
    pblock->hashUTXORoot = uint256(h256Touint(dev::h256(globalState->rootHashUTXO())));

    CalculateRewardWithoutDividents();
    AddRefundTransactions();

    if(fProofOfStake) {
        AddDividentsToCoinstakeTransaction();
        if(bceResult.contractAddresses.size() != 0) {
            AddTransactionForSavingContractOwners();
        }
    }

    ReplaceRewardTransaction();
    ////////////////////////////////////////////////////////

    pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, chainparams.GetConsensus(), fProofOfStake);
    pblocktemplate->vTxFees[0] = -nFees;

    // The total fee is the Fees minus the Refund
    if (pTotalFees)
        *pTotalFees = nFees - bceResult.refundSender;

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    if (!fProofOfStake)
        UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus(),fProofOfStake);
    pblock->nNonce         = 0;
    pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);

    CValidationState state;
    if (!fProofOfStake && !TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
    }

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries& testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        }
        else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOpsCost) const
{
    // TODO: switch to weight-based accounting for packages instead of vsize-based accounting.
    if (nBlockWeight + WITNESS_SCALE_FACTOR * packageSize >= nBlockMaxWeight)
        return false;
    if (nBlockSigOpsCost + packageSigOpsCost >= (uint64_t)dgpMaxBlockSigOps)
        return false;
    return true;
}

bool BlockAssembler::CheckTransactionLydraSpending(const CTxMemPool::setEntries& package)
{
    for (CTxMemPool::txiter it : package) {
        auto tx = it->GetTx();
        std::map<CTxDestination, CAmount> addresses_inputs;
        std::map<CTxDestination, CAmount> addresses_outputs;
        std::set<std::pair<uint256, int>> addresses_index;
        std::map<uint256, CTxDestination> addrhash_dest;
		std::set<uint256> addresses_index_checked;

        for (const CTxIn& txin : tx.vin) {
            CTxDestination dest;
            CCoinsViewCache view(pcoinsTip.get());
            const CTxOut& prevout = view.GetOutputFor(txin);
            if (ExtractDestination(txin.prevout, prevout.scriptPubKey, dest)) {
                uint256 hashBytes;
                int type = 0;
                if (!DecodeIndexKey(EncodeDestination(dest), hashBytes, type)) {
                    return false;
                }
                addresses_index.insert(std::make_pair(hashBytes, type));
                addrhash_dest[hashBytes] = dest;
                if (addresses_inputs.find(dest) != addresses_inputs.end())
                    addresses_inputs[dest] += prevout.nValue;
                else
                    addresses_inputs[dest] = prevout.nValue;

                if(!addresses_balances.count(dest)) {
                    // Get address utxos
                    std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > unspentOutputs;
                    if (!GetAddressUnspent(hashBytes, type, unspentOutputs)) {
                        return false;
                    }

                    // Add the utxos to the list if they are mature and at least the minimum value
                    int coinbaseMaturity = Params().GetConsensus().CoinbaseMaturity(chainActive.Height() + 1);
                    CAmount rembalance = 0;
                    for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> >::const_iterator i=unspentOutputs.begin(); i!=unspentOutputs.end(); i++) {

                        int nDepth = chainActive.Height() - i->second.blockHeight + 1;
                        if (i->second.coinStake && nDepth < coinbaseMaturity)
                            continue;

                        rembalance += i->second.satoshis;
                    }

                    addresses_balances.insert({dest, rembalance});
                }
            } else {
                return false;
            }
        }

        for (size_t j = 0; j < tx.vout.size(); j++) {
            const CTxOut& out = tx.vout[j];
            CTxDestination dest;
            if (ExtractDestination(out.scriptPubKey, dest)) {	
                if (addresses_outputs.find(dest) != addresses_outputs.end())
                    addresses_outputs[dest] += out.nValue;
                else
                    addresses_outputs[dest] = out.nValue;
            }
        }

        for (const auto& addr_pair : addresses_index) {
            // if(addresses_balances.count(addrhash_dest[addr_pair.first]) && 
            //     addresses_inputs.count(addrhash_dest[addr_pair.first]) &&
            //     addresses_outputs.count(addrhash_dest[addr_pair.first])) {
            auto all_inputs = addresses_inputs[addrhash_dest[addr_pair.first]];
            auto all_outputs = addresses_outputs[addrhash_dest[addr_pair.first]];
            Lydra l;
            uint64_t locked_hydra_amount;
            l.getLockedHydraAmountPerAddress(boost::get<CKeyID>(&addrhash_dest[addr_pair.first])->GetReverseHex(), locked_hydra_amount);
            if (!addresses_index_checked.count(addr_pair.first) && addresses_balances[addrhash_dest[addr_pair.first]] - all_inputs + all_outputs < locked_hydra_amount) {
                return false;
            }
            if (!addresses_index_checked.count(addr_pair.first)) {
                addresses_balances[addrhash_dest[addr_pair.first]] = 
                    addresses_balances[addrhash_dest[addr_pair.first]] - all_inputs + all_outputs;
                addresses_index_checked.insert(addr_pair.first);
            }
            // }
        }
    }

    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - premature witness (in case segwit transactions are added to mempool before
//   segwit activation)
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries& package)
{
    for (CTxMemPool::txiter it : package) {
        if (!IsFinalTx(it->GetTx(), nHeight, nLockTimeCutoff))
            return false;
        if (!fIncludeWitness && it->GetTx().HasWitness())
            return false;
    }
    return true;
}

bool BlockAssembler::AttemptToAddContractToBlock(CTxMemPool::txiter iter, uint64_t minGasPrice) {
    /*if (nTimeLimit != 0 && GetAdjustedTime() >= nTimeLimit - nBytecodeTimeBuffer) {
		LogPrintf("FAIL BYTECODE BUFFER\n");
        return false;
    }*/
    if (gArgs.GetBoolArg("-disablecontractstaking", false))
    {
        // Contract staking is disabled for the staker
        return false;
    }
    
    dev::h256 oldHashStateRoot(globalState->rootHash());
    dev::h256 oldHashUTXORoot(globalState->rootHashUTXO());
    // operate on local vars first, then later apply to `this`
    uint64_t nBlockWeight = this->nBlockWeight;
    uint64_t nBlockSigOpsCost = this->nBlockSigOpsCost;

    unsigned int contractflags = GetContractScriptFlags(nHeight, chainparams.GetConsensus());
    QtumTxConverter convert(iter->GetTx(), NULL, &pblock->vtx, contractflags);

    ExtractQtumTX resultConverter;
    if(!convert.extractionQtumTransactions(resultConverter)){
        //this check already happens when accepting txs into mempool
        //therefore, this can only be triggered by using raw transactions on the staker itself
        LogPrintf("AttemptToAddContractToBlock(): Fail to extract contacts from tx %s\n", iter->GetTx().GetHash().ToString());
        return false;
    }
    std::vector<QtumTransaction> qtumTransactions = resultConverter.first;
    dev::u256 txGas = 0;
    for(QtumTransaction &qtumTransaction : qtumTransactions){
        qtumTransaction.setTransactionFee(iter->GetFee());

        txGas += qtumTransaction.gas();
        if(txGas > txGasLimit) {
            // Limit the tx gas limit by the soft limit if such a limit has been specified.
            LogPrintf("AttemptToAddContractToBlock(): The gas needed is bigger than -staker-max-tx-gas-limit for the contract tx %s\n", iter->GetTx().GetHash().ToString());
            return false;
        }

        if(bceResult.usedGas + qtumTransaction.gas() > softBlockGasLimit){
            // If this transaction's gasLimit could cause block gas limit to be exceeded, then don't add it
            // Log if the contract is the only contract tx
            if(bceResult.usedGas == 0)
                LogPrintf("AttemptToAddContractToBlock(): The gas needed is bigger than -staker-soft-block-gas-limit for the contract tx %s\n", iter->GetTx().GetHash().ToString());
            return false;
        }
        if(qtumTransaction.gasPrice() < minGasPrice){
            //if this transaction's gasPrice is less than the current DGP minGasPrice don't add it
            LogPrintf("AttemptToAddContractToBlock(): The gas price is less than -staker-min-tx-gas-price for the contract tx %s\n", iter->GetTx().GetHash().ToString());
            return false;
        }
    }
    // We need to pass the DGP's block gas limit (not the soft limit) since it is consensus critical.
    ByteCodeExec exec(*pblock, qtumTransactions, hardBlockGasLimit, chainActive.Tip());
    if(!exec.performByteCode()){
        //error, don't add contract
        globalState->setRoot(oldHashStateRoot);
        globalState->setRootUTXO(oldHashUTXORoot);
        LogPrintf("AttemptToAddContractToBlock(): Perform byte code fails for the contract tx %s\n", iter->GetTx().GetHash().ToString());
        return false;
    }

    ByteCodeExecResult testExecResult;
    if(!exec.processingResults(testExecResult)){
        globalState->setRoot(oldHashStateRoot);
        globalState->setRootUTXO(oldHashUTXORoot);
        LogPrintf("AttemptToAddContractToBlock(): Processing results fails for the contract tx %s\n", iter->GetTx().GetHash().ToString());
        return false;
    }

    if(bceResult.usedGas + testExecResult.usedGas > softBlockGasLimit){
        // If this transaction could cause block gas limit to be exceeded, then don't add it
        globalState->setRoot(oldHashStateRoot);
        globalState->setRootUTXO(oldHashUTXORoot);
        // Log if the contract is the only contract tx
        if(bceResult.usedGas == 0)
            LogPrintf("AttemptToAddContractToBlock(): The gas used is bigger than -staker-soft-block-gas-limit for the contract tx %s\n", iter->GetTx().GetHash().ToString());
        return false;
    }

    //apply contractTx costs to local state
    nBlockWeight += iter->GetTxWeight();
    nBlockSigOpsCost += iter->GetSigOpCost();
    //apply value-transfer txs to local state
    for (CTransaction &t : testExecResult.valueTransfers) {
        nBlockWeight += GetTransactionWeight(t);
        nBlockSigOpsCost += GetLegacySigOpCount(t);
    }

    int proofTx = pblock->IsProofOfStake() ? 1 : 0;

    //calculate sigops from new refund/proof tx

    //first, subtract old proof tx
    nBlockSigOpsCost -= GetLegacySigOpCount(*pblock->vtx[proofTx]);

    // manually rebuild refundtx
    CMutableTransaction contrTx(*pblock->vtx[proofTx]);
    //note, this will need changed for MPoS
    int i=contrTx.vout.size();
    contrTx.vout.resize(contrTx.vout.size()+testExecResult.refundOutputs.size());
    for(CTxOut& vout : testExecResult.refundOutputs){
        contrTx.vout[i]=vout;
        i++;
    }
    nBlockSigOpsCost += GetLegacySigOpCount(contrTx);
    //all contract costs now applied to local state

    //Check if block will be too big or too expensive with this contract execution
    if (nBlockSigOpsCost * WITNESS_SCALE_FACTOR > (uint64_t)dgpMaxBlockSigOps ||
            nBlockWeight > dgpMaxBlockWeight) {
        //contract will not be added to block, so revert state to before we tried
        globalState->setRoot(oldHashStateRoot);
        globalState->setRootUTXO(oldHashUTXORoot);
		LogPrintf("FAIL GLOBAL STATE\n");
        return false;
    }

    //block is not too big, so apply the contract execution and it's results to the actual block

    //apply local bytecode to global bytecode state
    auto refundOutputs = testExecResult.refundOutputs;

    for (const auto &pair : exec.dividendByContract) {
        if (bceResult.dividendByContract.count(pair.first)) {
            bceResult.dividendByContract[pair.first] += pair.second;
        }
    }

    bceResult.contractAddresses.insert(
            std::end(bceResult.contractAddresses),
            std::begin(testExecResult.contractAddresses),
            std::end(testExecResult.contractAddresses));
    bceResult.contractOwners.insert(
            std::end(bceResult.contractOwners),
            std::begin(testExecResult.contractOwners),
            std::end(testExecResult.contractOwners));
    bceResult.dividendByContract.insert(
            std::begin(exec.dividendByContract),
            std::end(exec.dividendByContract));
    bceResult.usedGas += testExecResult.usedGas;
    bceResult.refundSender += testExecResult.refundSender;
    bceResult.refundOutputs.insert(bceResult.refundOutputs.end(), testExecResult.refundOutputs.begin(), testExecResult.refundOutputs.end());
    bceResult.valueTransfers = std::move(testExecResult.valueTransfers);

    pblock->vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    this->nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    this->nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    for (CTransaction &t : bceResult.valueTransfers) {
        pblock->vtx.emplace_back(MakeTransactionRef(std::move(t)));
        this->nBlockWeight += GetTransactionWeight(t);
        this->nBlockSigOpsCost += GetLegacySigOpCount(t);
        ++nBlockTx;
    }
    //calculate sigops from new refund/proof tx
    this->nBlockSigOpsCost -= GetLegacySigOpCount(*pblock->vtx[proofTx]);
    this->nBlockSigOpsCost += GetLegacySigOpCount(*pblock->vtx[proofTx]);

    bceResult.valueTransfers.clear();

    return true;
}


void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblock->vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf("fee %s txid %s\n",
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

int BlockAssembler::UpdatePackagesForAdded(const CTxMemPool::setEntries& alreadyAdded,
        indexed_modified_transaction_set &mapModifiedTx)
{
    int nDescendantsUpdated = 0;
    for (CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc))
                continue;
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCostWithAncestors -= it->GetSigOpCost();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }
    return nDescendantsUpdated;
}

// Skip entries in mapTx that are already in a block or are present
// in mapModifiedTx (which implies that the mapTx ancestor state is
// stale due to ancestor inclusion in the block)
// Also skip transactions that we've already failed to add. This can happen if
// we consider a transaction in mapModifiedTx and it fails: we can then
// potentially consider it again while walking mapTx.  It's currently
// guaranteed to fail again, but as a belt-and-suspenders check we put it in
// failedTx and avoid re-evaluation, since the re-evaluation would be using
// cached size/sigops/fee values that are not actually correct.
bool BlockAssembler::SkipMapTxEntry(CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx, CTxMemPool::setEntries &failedTx)
{
    assert (it != mempool.mapTx.end());
    return mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it);
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, std::vector<CTxMemPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs(int &nPackagesSelected, int &nDescendantsUpdated, uint64_t minGasPrice)
{
    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    CTxMemPool::indexed_transaction_set::index<ancestor_score_or_gas_price>::type::iterator mi = mempool.mapTx.get<ancestor_score_or_gas_price>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;
    addresses_balances.clear();
    while ((mi != mempool.mapTx.get<ancestor_score_or_gas_price>().end() || !mapModifiedTx.empty()) && nPackagesSelected < 65000)
    {	
        if(nTimeLimit != 0 && GetAdjustedTime() >= nTimeLimit){
            //no more time to add transactions, just exit
            return;
        }
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != mempool.mapTx.get<ancestor_score_or_gas_price>().end() &&
                SkipMapTxEntry(mempool.mapTx.project<0>(mi), mapModifiedTx, failedTx)) {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score_or_gas_price>().begin();
        if (mi == mempool.mapTx.get<ancestor_score_or_gas_price>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score_or_gas_price>().end() &&
                    CompareModifiedEntry()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOpsCost = iter->GetSigOpCostWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOpsCost = modit->nSigOpCostWithAncestors;
        }

        if (packageFees < blockMinFeeRate.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOpsCost)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score_or_gas_price>().erase(modit);
                failedTx.insert(iter);
            }

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight >
                    nBlockMaxWeight - 4000) {
                // Give up if we're close to full and haven't succeeded in a while
                break;
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final
        if (!TestPackageTransactions(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score_or_gas_price>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        if (!CheckTransactionLydraSpending(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score_or_gas_price>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }


        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, sortedEntries);

        bool wasAdded=true;
        for (size_t i=0; i<sortedEntries.size(); ++i) {
            if(!wasAdded || (nTimeLimit != 0 && GetAdjustedTime() >= nTimeLimit))
            {
                //if out of time, or earlier ancestor failed, then skip the rest of the transactions
                mapModifiedTx.erase(sortedEntries[i]);
                wasAdded=false;
                continue;
            }
            const CTransaction& tx = sortedEntries[i]->GetTx();
            if(wasAdded) {
                if (tx.HasCreateOrCall()) {
                    wasAdded = AttemptToAddContractToBlock(sortedEntries[i], minGasPrice);
                    if(!wasAdded){
                        if(fUsingModified) {
                            //this only needs to be done once to mark the whole package (everything in sortedEntries) as failed
                            mapModifiedTx.get<ancestor_score_or_gas_price>().erase(modit);
                            failedTx.insert(iter);
                        }
                    }
                } else {
                    AddToBlock(sortedEntries[i]);
                }
            }
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

        if(!wasAdded){
            //skip UpdatePackages if a transaction failed to be added (match TestPackage logic)
            continue;
        }

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(ancestors, mapModifiedTx);
    }
}

void IncrementExtraNonce(CBlock* pblock, const CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}

#ifdef ENABLE_WALLET
//////////////////////////////////////////////////////////////////////////////
//
// Proof of Stake miner
//

//
// Looking for suitable coins for creating new block.
//

class DelegationFilterBase : public IDelegationFilter
{
public:
    bool GetKey(const std::string& strAddress, uint160& keyId)
    {
        CTxDestination destination = DecodeDestination(strAddress);
        if (!IsValidDestination(destination)) {
            return false;
        }

        const CKeyID *keyid = boost::get<CKeyID>(&destination);
        if (!keyid) {
            return false;
        }

        keyId = uint160(*keyid);

        return true;
    }
};

class DelegationsStaker : public DelegationFilterBase
{
public:
    enum StakerType
    {
        STAKER_NORMAL    = 0,
        STAKER_WHITELIST = 1,
        STAKER_BLACKLIST = 2,
    };
    DelegationsStaker(CWallet *_pwallet):
        pwallet(_pwallet),
        cacheHeight(0),
        type(StakerType::STAKER_NORMAL)
    {
        // Get white list
        for (const std::string& strAddress : gArgs.GetArgs("-stakingwhitelist"))
        {
            uint160 keyId;
            if(GetKey(strAddress, keyId))
            {
                if(std::find(whiteList.begin(), whiteList.end(), keyId) == whiteList.end())
                    whiteList.push_back(keyId);
            }
            else
            {
                LogPrint(BCLog::COINSTAKE, "Fail to add %s to stake white list\n", strAddress);
            }
        }

        // Get black list
        for (const std::string& strAddress : gArgs.GetArgs("-stakingblacklist"))
        {
            uint160 keyId;
            if(GetKey(strAddress, keyId))
            {
                if(std::find(blackList.begin(), blackList.end(), keyId) == blackList.end())
                    blackList.push_back(keyId);
            }
            else
            {
                LogPrint(BCLog::COINSTAKE, "Fail to add %s to stake black list\n", strAddress);
            }
        }

        // Set staker type
        if(whiteList.size() > 0)
        {
            type = StakerType::STAKER_WHITELIST;
        }
        else if(blackList.size() > 0)
        {
            type = StakerType::STAKER_BLACKLIST;
        }
    }

    bool Match(const DelegationEvent& event) const
    {
        bool mine = pwallet->HaveKey(CKeyID(event.item.staker));
        if(!mine)
            return false;

        CSuperStakerInfo info;
        if(pwallet->GetSuperStaker(info, event.item.staker) && info.fCustomConfig)
        {
            return CheckAddressList(info.nDelegateAddressType, info.delegateAddressList, info.delegateAddressList, event);
        }

        return CheckAddressList(type, whiteList, blackList, event);
    }

    bool CheckAddressList(const int& _type, const std::vector<uint160>& _whiteList, const std::vector<uint160>& _blackList, const DelegationEvent& event) const
    {
        switch (_type) {
            case STAKER_NORMAL:
                return true;
            case STAKER_WHITELIST:
                return std::count(_whiteList.begin(), _whiteList.end(), event.item.delegate);
            case STAKER_BLACKLIST:
                return std::count(_blackList.begin(), _blackList.end(), event.item.delegate) == 0;
            default:
                break;
        }

        return false;
    }

    void Update(int32_t nHeight)
    {
        if(pwallet->fUpdatedSuperStaker)
        {
            // Clear cache if updated
            cacheHeight = 0;
            cacheDelegationsStaker.clear();
            pwallet->fUpdatedSuperStaker = false;
        }

        std::map<uint160, Delegation> delegations_staker;
        int checkpointSpan = Params().GetConsensus().CheckpointSpan(nHeight);
        if(nHeight <= checkpointSpan)
        {
            // Get delegations from events
            std::vector<DelegationEvent> events;
            qtumDelegations.FilterDelegationEvents(events, *this);
            delegations_staker = qtumDelegations.DelegationsFromEvents(events);
        }
        else
        {
            // Update the cached delegations for the staker, older then the sync checkpoint (500 blocks)
            int cpsHeight = nHeight - checkpointSpan;
            if(cacheHeight < cpsHeight)
            {
                std::vector<DelegationEvent> events;
                qtumDelegations.FilterDelegationEvents(events, *this, cacheHeight, cpsHeight);
                qtumDelegations.UpdateDelegationsFromEvents(events, cacheDelegationsStaker);
                cacheHeight = cpsHeight;
            }

            // Update the wallet delegations
            std::vector<DelegationEvent> events;
            qtumDelegations.FilterDelegationEvents(events, *this, cacheHeight + 1);
            delegations_staker = cacheDelegationsStaker;
            qtumDelegations.UpdateDelegationsFromEvents(events, delegations_staker);
        }
        pwallet->updateDelegationsStaker(delegations_staker);
    }

    void UpdateDelegationsAddress()
    {
        qtumDelegations.UpdateDelegationsAddress();
    }

private:
    CWallet *pwallet;
    QtumDelegation qtumDelegations;
    int32_t cacheHeight;
    std::map<uint160, Delegation> cacheDelegationsStaker;
    std::vector<uint160> whiteList;
    std::vector<uint160> blackList;
    int type;
};

class MyDelegations : public DelegationFilterBase
{
public:
    MyDelegations(CWallet *_pwallet):
        pwallet(_pwallet),
        cacheHeight(0),
        cacheAddressHeight(0)
    {}

    bool Match(const DelegationEvent& event) const
    {
        return pwallet->HaveKey(CKeyID(event.item.delegate));
    }

    void Update(interfaces::Chain::Lock& locked_chain, int32_t nHeight)
    {
        if(fLogEvents)
        {
            // When log events are enabled, search the log events to get complete list of my delegations
            int checkpointSpan = Params().GetConsensus().CheckpointSpan(nHeight);
            if(nHeight <= checkpointSpan)
            {
                // Get delegations from events
                std::vector<DelegationEvent> events;
                qtumDelegations.FilterDelegationEvents(events, *this);
                pwallet->m_my_delegations = qtumDelegations.DelegationsFromEvents(events);
            }
            else
            {
                // Update the cached delegations for the staker, older then the sync checkpoint (500 blocks)
                int cpsHeight = nHeight - checkpointSpan;
                if(cacheHeight < cpsHeight)
                {
                    std::vector<DelegationEvent> events;
                    qtumDelegations.FilterDelegationEvents(events, *this, cacheHeight, cpsHeight);
                    qtumDelegations.UpdateDelegationsFromEvents(events, cacheMyDelegations);
                    cacheHeight = cpsHeight;
                }

                // Update the wallet delegations
                std::vector<DelegationEvent> events;
                qtumDelegations.FilterDelegationEvents(events, *this, cacheHeight + 1);
                pwallet->m_my_delegations = cacheMyDelegations;
                qtumDelegations.UpdateDelegationsFromEvents(events, pwallet->m_my_delegations);
            }
        }
        else
        {
            // Log events are not enabled, search the available addresses for list of my delegations
            if(cacheHeight != nHeight)
            {
                cacheMyDelegations.clear();

                // Address map
                std::map<uint160, bool> mapAddress;

                // Get all addreses with coins
                SelectAddress(locked_chain, mapAddress, nHeight);

                // Get all addreses for delegations in the GUI
                for(auto item : pwallet->mapDelegation)
                {
                    uint160 address = item.second.delegateAddress;
                    if(pwallet->HaveKey(CKeyID(address)))
                    {
                        if (mapAddress.find(address) == mapAddress.end())
                        {
                            mapAddress[address] = false;
                        }
                    }
                }

                // Search my delegations in the addresses
                for(auto item: mapAddress)
                {
                    Delegation delegation;
                    uint160 address = item.first;
                    if(qtumDelegations.GetDelegation(address, delegation) && QtumDelegation::VerifyDelegation(address, delegation))
                    {
                        cacheMyDelegations[address] = delegation;
                    }
                }

                // Update my delegations list
                pwallet->m_my_delegations = cacheMyDelegations;
                cacheHeight = nHeight;
            }
        }
    }

    void UpdateDelegationsAddress()
    {
        qtumDelegations.UpdateDelegationsAddress();
    }

    void SelectAddress(interfaces::Chain::Lock& locked_chain, std::map<uint160, bool>& mapAddress, int32_t nHeight)
    {
        if(cacheAddressHeight < nHeight)
        {
            pwallet->SelectAddress(locked_chain, mapAddress);
            pwallet->mapAddressUnspentCache = mapAddress;
            if(pwallet->fUpdateAddressUnspentCache == false)
                pwallet->fUpdateAddressUnspentCache = true;
            cacheAddressHeight = nHeight + 100;
        }
        else
        {
            mapAddress = pwallet->mapAddressUnspentCache;
        }
    }

private:

    CWallet *pwallet;
    QtumDelegation qtumDelegations;
    int32_t cacheHeight;
    int32_t cacheAddressHeight;
    std::map<uint160, Delegation> cacheMyDelegations;
};

bool CheckStake(const std::shared_ptr<const CBlock> pblock, CWallet& wallet)
{
    uint256 proofHash, hashTarget;
    uint256 hashBlock = pblock->GetHash();

    if(!pblock->IsProofOfStake())
        return error("CheckStake() : %s is not a proof-of-stake block", hashBlock.GetHex());

    // verify hash target and signature of coinstake tx
    {
        auto locked_chain = wallet.chain().lock();
        CValidationState state;
        if (!CheckProofOfStake(mapBlockIndex[pblock->hashPrevBlock], state, *pblock->vtx[1], pblock->nBits, pblock->nTime, pblock->GetProofOfDelegation(), pblock->prevoutStake, proofHash, hashTarget, *pcoinsTip))
            return error("CheckStake() : proof-of-stake checking failed");
    }

    //// debug print
    LogPrint(BCLog::COINSTAKE, "CheckStake() : new proof-of-stake block found  \n  hash: %s \nproofhash: %s  \ntarget: %s\n", hashBlock.GetHex(), proofHash.GetHex(), hashTarget.GetHex());
    LogPrint(BCLog::COINSTAKE, "%s\n", pblock->ToString());
    LogPrint(BCLog::COINSTAKE, "out %s\n", FormatMoney(pblock->vtx[1]->GetValueOut()));

    // Found a solution
    {
        auto locked_chain = wallet.chain().lock();
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())
            return error("CheckStake() : generated block is stale");

        LOCK(wallet.cs_wallet);
        for(const CTxIn& vin : pblock->vtx[1]->vin) {
            if (wallet.IsSpent(*locked_chain, vin.prevout.hash, vin.prevout.n)) {
                return error("CheckStake() : generated block became invalid due to stake UTXO being spent");
            }
        }
    }

    // Process this block the same as if we had received it from another node
    bool fNewBlock = false;
    if (!ProcessNewBlock(Params(), pblock, true, &fNewBlock))
        return error("CheckStake() : ProcessBlock, block not accepted");

    return true;
}

bool SleepStaker(CWallet *pwallet, u_int64_t milliseconds)
{
    u_int64_t seconds = milliseconds / 1000;
    milliseconds %= 1000;

    for(unsigned int i = 0; i < seconds; i++)
    {
        if(!pwallet->IsStakeClosing())
            UninterruptibleSleep(std::chrono::seconds{1});
        else
            return false;
    }

    if(milliseconds)
    {
        if(!pwallet->IsStakeClosing())
            UninterruptibleSleep(std::chrono::milliseconds{milliseconds});
        else
            return false;
    }

    return !pwallet->IsStakeClosing();
}

/**
 * @brief The IStakeMiner class Miner interface
 */
class IStakeMiner
{
public:
    /**
     * @brief init Initialize the miner
     * @param pwallet Wallet to use
     * @param connman Nodes that connect
     */
    virtual void Init(CWallet *pwallet, CConnman* connman) = 0;

    /**
     * @brief run Run the miner
     */
    virtual void Run() = 0;

    /**
     * @brief ~IStakeMiner Destructor
     */
    virtual ~IStakeMiner() {};
};

class SolveItem
{
public:
    SolveItem(const COutPoint& _prevoutStake, const uint32_t& _blockTime, const bool& _delegate):
        prevoutStake(_prevoutStake),
        blockTime(_blockTime),
        delegate(_delegate)
    {}

    COutPoint prevoutStake;
    uint32_t blockTime = 0;
    bool delegate = false;
};

class StakeMinerPriv
{
public:
    CWallet *pwallet = 0;
    CConnman* connman = 0;
    bool fTryToSync = true;
    bool regtestMode = false;
    bool minDifficulty = false;
    bool fSuperStake = false;
    const Consensus::Params& consensusParams;
    int nOfflineStakeHeight = 0;
    bool fDelegationsContract = false;
    bool fEmergencyStaking = false;
    bool fAggressiveStaking = false;
    bool fError = false;
    int numThreads = 1;
    boost::thread_group threads;
    mutable RecursiveMutex cs_worker;

public:
    DelegationsStaker delegationsStaker;
    MyDelegations myDelegations;

public:
    int32_t nHeight = 0;
    uint32_t stakeTimestampMask = 1;
    int64_t nTotalFees = 0;
    bool haveCoinsForStake = false;
    bool forceUpdate = false;

    CBlockIndex* pindexPrev = 0;
    CAmount nTargetValue = 0;
    std::set<std::pair<const CWalletTx*,unsigned int> > setCoins;
    std::vector<COutPoint> setSelectedCoins;
    std::vector<COutPoint> setDelegateCoins;
    std::vector<COutPoint> prevouts;
    std::map<uint32_t, bool> mapSolveBlockTime;
    std::multimap<uint256, SolveItem> mapSolvedBlock;
    std::map<uint32_t, std::vector<COutPoint>> mapSolveSelectedCoins;
    std::map<uint32_t, std::vector<COutPoint>> mapSolveDelegateCoins;
    uint32_t beginningTime = 0;
    uint32_t endingTime = 0;
    uint32_t waitBestHeaderAttempts = 0;

    std::shared_ptr<CBlock> pblock;
    std::unique_ptr<CBlockTemplate> pblocktemplate;
    std::shared_ptr<CBlock> pblockfilled;
    std::unique_ptr<CBlockTemplate> pblocktemplatefilled;

public:
    StakeMinerPriv(CWallet *_pwallet, CConnman* _connman):
        pwallet(_pwallet),
        connman(_connman),
        consensusParams(Params().GetConsensus()),
        delegationsStaker(_pwallet),
        myDelegations(_pwallet)
    {
        // Make this thread recognisable as the mining thread
        std::string threadName = "hydrastake";
        if(pwallet && pwallet->GetName() != "")
        {
            threadName = threadName + "-" + pwallet->GetName();
        }
        RenameThread(threadName.c_str());

        regtestMode = Params().MineBlocksOnDemand();
        minDifficulty = consensusParams.fPowAllowMinDifficultyBlocks;
        fSuperStake = gArgs.GetBoolArg("-superstaking", DEFAULT_SUPER_STAKE);
        nOfflineStakeHeight = consensusParams.nOfflineStakeHeight;
        fDelegationsContract = !consensusParams.delegationsAddress.IsNull();
        fEmergencyStaking = gArgs.GetBoolArg("-emergencystaking", false);
        fAggressiveStaking = gArgs.IsArgSet("-aggressive-staking");
        int maxWaitForBestHeader = gArgs.GetArg("-maxstakerwaitforbestheader", DEFAULT_MAX_STAKER_WAIT_FOR_BEST_BLOCK_HEADER);
        if(maxWaitForBestHeader > 0)
        {
            waitBestHeaderAttempts = maxWaitForBestHeader / nMinerWaitBestBlockHeader;
        }
        if(pwallet) numThreads = pwallet->m_num_threads;
    }

    void clearCache()
    {
        nHeight = 0;
        stakeTimestampMask = 1;
        nTotalFees = 0;
        haveCoinsForStake = false;
        forceUpdate = false;

        pindexPrev = 0;
        nTargetValue = 0;
        setCoins.clear();
        setSelectedCoins.clear();
        setDelegateCoins.clear();
        prevouts.clear();
        mapSolveBlockTime.clear();
        mapSolvedBlock.clear();
        mapSolveSelectedCoins.clear();
        mapSolveDelegateCoins.clear();
        beginningTime = 0;
        endingTime = 0;

        pblock.reset();
        pblocktemplate.reset();
        pblockfilled.reset();
        pblocktemplatefilled.reset();
    }
};

class StakeMiner : public IStakeMiner
{
private:
    StakeMinerPriv *d = 0;

public:
    void Init(CWallet *pwallet, CConnman* connman)
    {
        d = new StakeMinerPriv(pwallet, connman);
    }

    void Run()
    {
        SetThreadPriority(THREAD_PRIORITY_LOWEST);

        while (Next()) {
            if (d->fDelegationsContract) {
                if (chainActive.Height() >= Params().GetConsensus().nDelegationsGasFixHeight)
                {
                    d->delegationsStaker.UpdateDelegationsAddress();
                    d->myDelegations.UpdateDelegationsAddress();
                }
            }

            // Is ready for mining
            if(!IsReady()) continue;

            // Cache mining data
            if(!CacheData()) continue;

            // Check if miner have coins for staking
            if(HaveCoinsForStake())
            {
                // Look for possibility to create a block
                d->beginningTime = GetAdjustedTime();
                d->beginningTime &= ~d->stakeTimestampMask;
                d->endingTime = d->beginningTime + nMaxStakeLookahead;

                for(uint32_t blockTime = d->beginningTime; blockTime < d->endingTime; blockTime += d->stakeTimestampMask+1)
                {
                    // Update status bar
                    UpdateStatusBar(blockTime);

                    // Check cached data
                    if(IsCachedDataOld())
                        break;

                    // Check if block can be created
                    if(CanCreateBlock(blockTime))
                    {
                        // Create new block
                        if(!CreateNewBlock(blockTime)) break;

                        // Sign new block
                        if(SignNewBlock(blockTime)) break;
                    }
                }
            }

            // Miner sleep before the next try
            Sleep(nMinerSleep);
        }
    }

    ~StakeMiner()
    {
        if(d)
        {
            delete d;
            d = 0;
        }
    }

protected:
    bool Next()
    {
        return d && d->pwallet && !d->pwallet->IsStakeClosing() && !d->fError;
    }

    bool Sleep(u_int64_t milliseconds)
    {
        return SleepStaker(d->pwallet, milliseconds);
    }

    bool IsStale(std::shared_ptr<CBlock> pblock)
    {
        if(d->pwallet->IsStakeClosing())
            return false;

        auto locked_chain = d->pwallet->chain().lock();
        CBlockIndex* tip = chainActive.Tip();
        return tip != d->pindexPrev || tip->GetBlockHash() != pblock->hashPrevBlock;
    }

    bool IsReady()
    {
        // Check if wallet is ready
        while (d->pwallet->IsLocked() || !d->pwallet->m_enabled_staking || fReindex || fImporting)
        {
            d->pwallet->m_last_coin_stake_search_interval = 0;
            if(!Sleep(10000))
                return false;
        }

        // Check if cached data is old
        uint32_t blokTime = GetAdjustedTime();
        blokTime &= ~d->stakeTimestampMask;
        if(!IsCachedDataOld() && d->endingTime >= blokTime)
        {
            Sleep(100);
            return false;
        }

        // Wait for node connections
        // Don't disable PoS mining for no connections if in regtest mode
        if(!d->regtestMode && !d->fEmergencyStaking) {
            while (d->connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0 || IsInitialBlockDownload()) {
                d->pwallet->m_last_coin_stake_search_interval = 0;
                d->fTryToSync = true;
                if(!Sleep(1000))
                    return false;
            }
            if (d->fTryToSync) {
                d->fTryToSync = false;
                if (d->connman->GetNodeCount(CConnman::CONNECTIONS_ALL) < 3 ||
                    chainActive.Tip()->GetBlockTime() < GetTime() - 10 * 60) {
                    Sleep(60000);
                    return false;
                }
            }
        }

        // Wait for PoW block time in regtest mode
        if(d->regtestMode) {
            bool waitForBlockTime = false;
            {
                if(d->pwallet->IsStakeClosing()) return false;
                auto locked_chain = d->pwallet->chain().lock();
                CBlockIndex* tip = chainActive.Tip();
                if(tip && tip->IsProofOfWork() && tip->GetBlockTime() > GetTime()) {
                    waitForBlockTime = true;
                }
            }
            // Wait for generated PoW block time
            if(waitForBlockTime) {
                Sleep(10000);
                return false;
            }
        }

        return true;
    }

    bool IsCachedDataOld()
    {
        if(d->pwallet->IsStakeClosing()) return false;
        if(d->pindexPrev == 0 || d->forceUpdate) return true;
        auto locked_chain = d->pwallet->chain().lock();
        return chainActive.Tip() != d->pindexPrev;
    }

    bool WaitBestHeader()
    {
        if(d->pwallet->IsStakeClosing()) return false;
        if(d->fEmergencyStaking || d->fAggressiveStaking) return false;
        auto locked_chain = d->pwallet->chain().lock();
        CBlockIndex* tip = chainActive.Tip();
        if(pindexBestHeader!= 0 &&
                tip != 0 &&
                tip != pindexBestHeader &&
                tip->nHeight < pindexBestHeader->nHeight)
        {
            return true;
        }

        return false;
    }

    bool SyncWithMiners()
    {
        // Try sync with mines
        for(size_t i = 0; i < d->waitBestHeaderAttempts; i++)
        {
            if(WaitBestHeader())
            {
                if(!Sleep(nMinerWaitBestBlockHeader))
                    return false;
            }
            else
            {
                break;
            }
        }

        return true;
    }

    bool UpdateData()
    {
        if(d->pwallet->IsStakeClosing()) return false;
        auto locked_chain = d->pwallet->chain().lock();
        LOCK(d->pwallet->cs_wallet);

        d->clearCache();
        CAmount nBalance = d->pwallet->GetBalance();
        d->nTargetValue = nBalance - d->pwallet->m_reserve_balance;
        CAmount nValueIn = 0;
        d->pindexPrev = chainActive.Tip();
        int32_t nHeightTip = chainActive.Height();
        d->nHeight = nHeightTip + 1;
        updateMinerParams(d->nHeight, d->consensusParams, d->minDifficulty);
        bool fOfflineStakeEnabled = (d->nHeight > d->nOfflineStakeHeight) && d->fDelegationsContract;
        if(fOfflineStakeEnabled)
        {
            d->myDelegations.Update(*locked_chain, nHeightTip);
        }
        d->pwallet->SelectCoinsForStaking(*locked_chain, d->nTargetValue, d->setCoins, nValueIn);
        if(d->fSuperStake && fOfflineStakeEnabled)
        {
            d->delegationsStaker.Update(nHeightTip);
            std::map<uint160, CAmount> mDelegateWeight;
            d->pwallet->SelectDelegateCoinsForStaking(*locked_chain, d->setDelegateCoins, mDelegateWeight);
            d->pwallet->updateDelegationsWeight(mDelegateWeight);
            d->pwallet->updateHaveCoinSuperStaker(d->setCoins);
        }
        d->stakeTimestampMask = d->consensusParams.StakeTimestampMask(d->nHeight);

        d->haveCoinsForStake = d->setCoins.size() > 0 || d->pwallet->CanSuperStake(d->setCoins, d->setDelegateCoins);
        if(d->haveCoinsForStake)
        {
            // Create an empty block. No need to process transactions until we know we can create a block
            d->nTotalFees = 0;
            d->pblocktemplate = std::unique_ptr<CBlockTemplate>(BlockAssembler(Params(), d->pwallet).CreateEmptyBlock(CScript(), true, true, &d->nTotalFees));
            if (!d->pblocktemplate.get()) {
                d->fError = true;
                return false;
            }
            d->pblock = std::make_shared<CBlock>(d->pblocktemplate->block);

            d->prevouts.insert(d->prevouts.end(), d->setDelegateCoins.begin(), d->setDelegateCoins.end());
            for(const std::pair<const CWalletTx*,unsigned int> &pcoin : d->setCoins)
            {
                d->prevouts.push_back(COutPoint(pcoin.first->GetHash(), pcoin.second));
            }

            d->pwallet->UpdateMinerStakeCache(true, d->prevouts, d->pindexPrev);
        }

        d->beginningTime = GetAdjustedTime();
        d->beginningTime &= ~d->stakeTimestampMask;
        d->endingTime = d->beginningTime + nMaxStakeLookahead;

        return true;
    }

    bool CacheData()
    {
        if(IsCachedDataOld())
        {
            if(!UpdateData())
                return false;
        }

        return !d->pwallet->IsStakeClosing();
    }

    bool HaveCoinsForStake()
    {
        return d->haveCoinsForStake;
    }

    void UpdateStatusBar(const uint32_t& blockTime)
    {
        // The information is needed for status bar to determine if the staker is trying to create block and when it will be created approximately,
        if(d->pwallet->m_last_coin_stake_search_time == 0) d->pwallet->m_last_coin_stake_search_time = GetAdjustedTime(); // startup timestamp
        // nLastCoinStakeSearchInterval > 0 mean that the staker is running
        int64_t searchInterval = blockTime - d->pwallet->m_last_coin_stake_search_time;
        if(searchInterval > 0) d->pwallet->m_last_coin_stake_search_interval = searchInterval;
    }

    void SloveBlock(uint32_t blockTime, size_t delegateSize, size_t from, size_t to)
    {
        std::multimap<uint256, SolveItem> tmpSolvedBlock;
        for(size_t i = from; i < to; i++)
        {
            const COutPoint &prevoutStake = d->prevouts[i];
            uint256 hashProofOfStake;
            if (CheckKernelCache(d->pindexPrev, d->pblock->nBits, blockTime, prevoutStake, d->pwallet->minerStakeCache, hashProofOfStake))
            {
                bool delegate = i < delegateSize;
                tmpSolvedBlock.insert(std::make_pair(hashProofOfStake, SolveItem(prevoutStake, blockTime, delegate)));
            }
        }

        if(tmpSolvedBlock.size() > 0)
        {
            LOCK(d->cs_worker);
            d->mapSolveBlockTime[blockTime] = true;
            d->mapSolvedBlock.insert(tmpSolvedBlock.begin(), tmpSolvedBlock.end());
        }
    }

    void SloveBlock(const uint32_t& blockTime)
    {
        // Init variables
        size_t listSize = d->prevouts.size();
        size_t delegateSize = d->setDelegateCoins.size();

        // Solve block
        int numThreads = std::min(d->numThreads, (int)listSize);
        if(listSize < 1000 || numThreads < 2)
        {
            SloveBlock(blockTime, delegateSize, 0, listSize);
        }
        else
        {
            size_t chunk = listSize / numThreads;
            for(int i = 0; i < numThreads; i++)
            {
                size_t from = i * chunk;
                size_t to = i == (numThreads -1) ? listSize : from + chunk;
                d->threads.create_thread([this, blockTime, delegateSize, from, to]{SloveBlock(blockTime, delegateSize, from, to);});
            }
            d->threads.join_all();
        }

        // Populate the list with the potential solwed blocks
        for (auto it = d->mapSolvedBlock.begin(); it != d->mapSolvedBlock.end(); ++it)
        {
            const SolveItem& item = (*it).second;
            if(item.delegate)
            {
                d->mapSolveDelegateCoins[item.blockTime].push_back(item.prevoutStake);
            }
            else
            {
                d->mapSolveSelectedCoins[item.blockTime].push_back(item.prevoutStake);
            }
        }
    }

    bool CanCreateBlock(const uint32_t& blockTime)
    {
        d->pblock->nTime = blockTime;
        if(d->mapSolveBlockTime.find(blockTime) == d->mapSolveBlockTime.end())
        {
            d->mapSolveBlockTime[blockTime] = false;
            SloveBlock(blockTime);
        }

        return d->mapSolveBlockTime[blockTime];
    }

    bool CreateNewBlock(const uint32_t& blockTime)
    {
        // increase priority so we can build the full PoS block ASAP to ensure the timestamp doesn't expire
        SetThreadPriority(THREAD_PRIORITY_ABOVE_NORMAL);

        if (IsStale(d->pblock)) {
            //another block was received while building ours, scrap progress
            LogPrintf("ThreadStakeMiner(): Valid future PoS block was orphaned before becoming valid\n");
            return false;
        }

        // Try to create an empty PoS block to get the address of the block creator for contracts
        if (!SignBlock(d->pblock, *(d->pwallet), d->nTotalFees, blockTime, d->setCoins, d->mapSolveSelectedCoins[blockTime], d->mapSolveDelegateCoins[blockTime], true, true))
            return false;

        // Create a block that's properly populated with transactions
        d->pblocktemplatefilled = std::unique_ptr<CBlockTemplate>(
                BlockAssembler(Params(), d->pwallet).CreateNewBlock(d->pblock->vtx[1]->vout[1].scriptPubKey, true, true, &(d->nTotalFees),
                                                        blockTime, FutureDrift(GetAdjustedTime(), d->nHeight, d->consensusParams) - nStakeTimeBuffer,
                                                        d->pwallet, d->setCoins, d->mapSolveSelectedCoins[blockTime], d->mapSolveDelegateCoins[blockTime]));
        if (!d->pblocktemplatefilled.get()) {
            d->fError = true;
            return false;
        }

        if (IsStale(d->pblock)) {
            //another block was received while building ours, scrap progress
            LogPrintf("ThreadStakeMiner(): Valid future PoS block was orphaned before becoming valid\n");
            return false;
        }

        // Sign the full block and use the timestamp from earlier for a valid stake
        d->pblockfilled = std::make_shared<CBlock>(d->pblocktemplatefilled->block);

        return true;
    }

    bool SignNewBlock(const uint32_t& blockTime)
    {
        // Try to sign the block once at specific time with the same cached data
        d->mapSolveBlockTime[blockTime] = false;

        if (SignBlock(d->pblockfilled, *(d->pwallet), d->nTotalFees, blockTime, d->setCoins, d->mapSolveSelectedCoins[blockTime], d->mapSolveDelegateCoins[blockTime], true)) {
            // Should always reach here unless we spent too much time processing transactions and the timestamp is now invalid
            // CheckStake also does CheckBlock and AcceptBlock to propogate it to the network
            bool validBlock = false;
            while(!validBlock) {
                if (IsStale(d->pblockfilled)) {
                    //another block was received while building ours, scrap progress
                    LogPrintf("ThreadStakeMiner(): Valid future PoS block was orphaned before becoming valid\n");
                    break;
                }
                //check timestamps
                if (d->pblockfilled->GetBlockTime() <= d->pindexPrev->GetBlockTime() ||
                    FutureDrift(d->pblockfilled->GetBlockTime(), d->nHeight, d->consensusParams) < d->pindexPrev->GetBlockTime()) {
                    LogPrintf("ThreadStakeMiner(): Valid PoS block took too long to create and has expired\n");
                    break; //timestamp too late, so ignore
                }
                if (d->pblockfilled->GetBlockTime() > FutureDrift(GetAdjustedTime(), d->nHeight, d->consensusParams)) {
                    if (d->fAggressiveStaking) {
                        //if being agressive, then check more often to publish immediately when valid. This might allow you to find more blocks,
                        //but also increases the chance of broadcasting invalid blocks and getting DoS banned by nodes,
                        //or receiving more stale/orphan blocks than normal. Use at your own risk.
                        if(!Sleep(nMinerWaitWalidBlock)) break;
                    }else{
                        //too early, so wait 3 seconds and try again
                        if(!Sleep(nMinerWaitWalidBlock)) break;
                    }
                    continue;
                }
                //if there is mined block by other staker wait for it to download
                if(!SyncWithMiners()) break;
                validBlock=true;
            }
            if(validBlock) {
                if(!CheckStake(d->pblockfilled, *(d->pwallet)))
                    d->forceUpdate = true;
                // Update the search time when new valid block is created, needed for status bar icon
                d->pwallet->m_last_coin_stake_search_time = d->pblockfilled->GetBlockTime();
            }
            return true;
        }

        //return back to low priority
        SetThreadPriority(THREAD_PRIORITY_LOWEST);
        return false;
    }
};

IStakeMiner *createMiner()
{
    return new StakeMiner();
}

void ThreadStakeMiner(CWallet *pwallet, CConnman* connman)
{
    IStakeMiner* miner = createMiner();
    miner->Init(pwallet, connman);
    miner->Run();
    delete miner;
    miner = 0;
}

void StakeQtums(bool fStake, CWallet *pwallet, CConnman* connman, boost::thread_group*& stakeThread)
{
    if (stakeThread != nullptr)
    {
        stakeThread->interrupt_all();
        stakeThread->join_all();
        delete stakeThread;
        stakeThread = nullptr;
    }

    if(fStake)
    {
        stakeThread = new boost::thread_group();
        stakeThread->create_thread(boost::bind(&ThreadStakeMiner, pwallet, connman));
    }
}

void RefreshDelegates(CWallet *pwallet, bool refreshMyDelegates, bool refreshStakerDelegates)
{
    if(pwallet && (refreshMyDelegates || refreshStakerDelegates))
    {
        auto locked_chain = pwallet->chain().lock();
        LOCK(pwallet->cs_wallet);

        DelegationsStaker delegationsStaker(pwallet);
        MyDelegations myDelegations(pwallet);

        int nOfflineStakeHeight = Params().GetConsensus().nOfflineStakeHeight;
        bool fDelegationsContract = !Params().GetConsensus().delegationsAddress.IsNull();
        int32_t nHeight = chainActive.Height();
        bool fOfflineStakeEnabled = ((nHeight + 1) > nOfflineStakeHeight) && fDelegationsContract;
        if(fOfflineStakeEnabled)
        {
            if(refreshMyDelegates)
            {
                myDelegations.Update(*locked_chain, nHeight);
            }

            if(refreshStakerDelegates)
            {
                bool fUpdatedSuperStaker = pwallet->fUpdatedSuperStaker;
                delegationsStaker.Update(nHeight);
                pwallet->fUpdatedSuperStaker = fUpdatedSuperStaker;
            }
        }
    }
}
#endif
