// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>

extern bool CheckQIP9BlockTimeDiff(const CBlockIndex* pindex);

namespace {
    // returns a * exp(p/q) where |p/q| is small
    arith_uint256 mul_exp(arith_uint256 a, int64_t p, int64_t q)
    {
        bool isNegative = p < 0;
        uint64_t abs_p = p >= 0 ? p : -p;
        arith_uint256 result = a;
        uint64_t n = 0;
        while (a > 0) {
            ++n;
            a = a * abs_p / q / n;
            if (isNegative && (n % 2 == 1)) {
                result -= a;
            } else {
                result += a;
            }
        }
        return result;
    }
}

// ppcoin: find last block index up to pindex
const CBlockIndex* GetLastBlockIndex(const CBlockIndex* pindex, bool fProofOfStake)
{
    //CBlockIndex will be updated with information about the proof type later
    while (pindex && pindex->pprev && (pindex->IsProofOfStake() != fProofOfStake))
        pindex = pindex->pprev;
    return pindex;
}

inline arith_uint256 GetLimit(int nHeight, const Consensus::Params& params, bool fProofOfStake)
{
    if(fProofOfStake) {
        return UintToArith256(params.posLimit);
    } else {
        return UintToArith256(params.powLimit);
    }
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params, bool fProofOfStake)
{
    unsigned int  nTargetLimit = GetLimit(pindexLast ? pindexLast->nHeight+1 : 0, params, fProofOfStake).GetCompact();

    // genesis block
    if (pindexLast == NULL)
        return nTargetLimit;

    // first block
    const CBlockIndex* pindexPrev = GetLastBlockIndex(pindexLast, fProofOfStake);
    if (pindexPrev->pprev == NULL)
        return nTargetLimit;

    // second block
    const CBlockIndex* pindexPrevPrev = GetLastBlockIndex(pindexPrev->pprev, fProofOfStake);
    if (pindexPrevPrev->pprev == NULL)
        return nTargetLimit;

    // min difficulty
    if (params.fPowAllowMinDifficultyBlocks)
    {
        // Special difficulty rule for testnet:
        // If the new block's timestamp is more than 2* 10 minutes
        // then allow mining of a min-difficulty block.
        int nHeight = pindexLast->nHeight + 1;
        if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.TargetSpacing(nHeight)*2)
        {
            return nTargetLimit;
        }
        else
        {
            // Return the last non-special-min-difficulty-rules-block
            const CBlockIndex* pindex = pindexLast;
            bool ignore = CheckQIP9BlockTimeDiff(pindex);
            while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval(pindex->nHeight, ignore) != 0 && pindex->nBits == nTargetLimit)
                pindex = pindex->pprev;
            return pindex->nBits;
        }
        return pindexLast->nBits;
    }

    return CalculateNextWorkRequired(pindexPrev, pindexPrevPrev->GetBlockTime(), params, fProofOfStake);
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params, bool fProofOfStake)
{
    if(fProofOfStake){
        if (params.fPoSNoRetargeting)
            return pindexLast->nBits;
    }else{
        if (params.fPowNoRetargeting)
            return pindexLast->nBits;
    }
    // Limit adjustment step
    int nHeight = pindexLast->nHeight + 1;
    int64_t nTargetSpacing = params.TargetSpacing(nHeight);
    int64_t nActualSpacing = pindexLast->GetBlockTime() - nFirstBlockTime;
	// Retarget
    const arith_uint256 bnTargetLimit = GetLimit(nHeight, params, fProofOfStake);
    // ppcoin: target change every block
    // ppcoin: retarget with exponential moving toward target spacing
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bool ignore = CheckQIP9BlockTimeDiff(pindexLast);
    int64_t nInterval = params.DifficultyAdjustmentInterval(nHeight, ignore); 

    if (nHeight < params.QIP9Height || ignore) {
        if (nActualSpacing < 0)
            nActualSpacing = nTargetSpacing;
        if (nActualSpacing > nTargetSpacing * 10)
            nActualSpacing = nTargetSpacing * 10;
        bnNew *= ((nInterval - 1) * nTargetSpacing + nActualSpacing + nActualSpacing);
        bnNew /= ((nInterval + 1) * nTargetSpacing);
    } else {
        if (nActualSpacing < 0)
            nActualSpacing = nTargetSpacing;
        if (nActualSpacing > nTargetSpacing * 20)
            nActualSpacing = nTargetSpacing * 20;
        uint32_t stakeTimestampMask=params.StakeTimestampMask(nHeight);
        bnNew = mul_exp(bnNew, 2 * (nActualSpacing - nTargetSpacing) / (stakeTimestampMask + 1), (nInterval + 1) * nTargetSpacing / (stakeTimestampMask + 1));
    }

    if (bnNew <= 0 || bnNew > bnTargetLimit)
        bnNew = bnTargetLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params, bool fProofOfStake)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}
