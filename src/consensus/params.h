// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_PARAMS_H
#define BITCOIN_CONSENSUS_PARAMS_H

#include <uint256.h>
#include <limits>
#include <map>
#include <string>
#include "amount.h"

namespace Consensus {

enum DeploymentPos
{
    DEPLOYMENT_TESTDUMMY,
    DEPLOYMENT_CSV, // Deployment of BIP68, BIP112, and BIP113.
    DEPLOYMENT_SEGWIT, // Deployment of BIP141, BIP143, and BIP147.
    // NOTE: Also add new deployments to VersionBitsDeploymentInfo in versionbits.cpp
    MAX_VERSION_BITS_DEPLOYMENTS
};

/**
 * Struct for each individual consensus rule change using BIP9.
 */
struct BIP9Deployment {
    /** Bit position to select the particular bit in nVersion. */
    int bit;
    /** Start MedianTime for version bits miner confirmation. Can be a date in the past */
    int64_t nStartTime;
    /** Timeout/expiry MedianTime for the deployment attempt. */
    int64_t nTimeout;

    /** Constant for nTimeout very far in the future. */
    static constexpr int64_t NO_TIMEOUT = std::numeric_limits<int64_t>::max();

    /** Special value for nStartTime indicating that the deployment is always active.
     *  This is useful for testing, as it means tests don't need to deal with the activation
     *  process (which takes at least 3 BIP9 intervals). Only tests that specifically test the
     *  behaviour during activation cannot use this. */
    static constexpr int64_t ALWAYS_ACTIVE = -1;
};

/**
 * Parameters that influence chain consensus.
 */
struct Params {
    uint256 hashGenesisBlock;
    unsigned long long int nBlockRewardChangeInterval;
    /* Block hash that is excepted from BIP16 enforcement */
    uint256 BIP16Exception;
    /** Block height and hash at which BIP34 becomes active */
    int BIP34Height;
    uint256 BIP34Hash;
    /** Block height at which BIP65 becomes active */
    int BIP65Height;
    /** Block height at which BIP66 becomes active */
    int BIP66Height;
    /** Block height at which QIP5 becomes active */
    int QIP5Height;
    /** Block height at which QIP6 becomes active */
    int QIP6Height;
    /** Block height at which QIP7 becomes active */
    int QIP7Height;
    /** Block height at which QIP9 becomes active */
    int QIP9Height;
    /** Block height at which Muir Glacier fork becomse active*/
    int MuirGlacierHeight;
    /** Block height at which Offline Staking becomes active */
    int nOfflineStakeHeight;
    /** Block height at which Reduce Block Time becomes active */
    int nReduceBlocktimeHeight;
    /** Block height at which 4x block time reward fix becomes active */
    int nRewardFixHeight;
    /** Block height at which reward offset subtraction becomes active */
    int nRewardOffsetHeight;
    /** Reward offset amount */
    CAmount nRewardOffsetAmount;
    /** Block height at which contract outs <= 1 rule becomes active */
    int nContractOutsHeight;
    /** Block height at which lydra becomes active */
    int nLydraHeight;
    /** Block height at which delegations contract gas fix becomes active */
    int nDelegationsGasFixHeight;
    /** Block height at which refund fix becomes active */
    int nRefundFixHeight;
    /** Block height at which overspend fix becomes active */
    int nLydraOverspendingFixHeight;

    /**
     * Minimum blocks including miner confirmation of the total of 2016 blocks in a retargeting period,
     * (nPowTargetTimespan / nPowTargetSpacing) which is also used for BIP9 deployments.
     * Examples: 1916 for 95%, 1512 for testchains.
     */
    uint32_t nRuleChangeActivationThreshold;
    uint32_t nMinerConfirmationWindow;
    BIP9Deployment vDeployments[MAX_VERSION_BITS_DEPLOYMENTS];
    /** Proof of work parameters */
    uint256 powLimit;
    uint256 posLimit;
    uint256 QIP9PosLimit;
    uint256 RBTPosLimit;
    bool fPowAllowMinDifficultyBlocks;
    bool fPowNoRetargeting;
    bool fPoSNoRetargeting;
    int64_t nPowTargetSpacing;
    int64_t nRBTPowTargetSpacing;
    int64_t nPowTargetTimespan;
    int64_t nPowTargetTimespanV2;
    int64_t nRBTPowTargetTimespan;
    int64_t nRBTPowTargetTimespanV2;
    uint256 nMinimumChainWork;
    uint256 defaultAssumeValid;
    int nLastPOWBlock;
    CAmount totalCoinsSupply;
    CAmount initialCoinsSupply;
    int blocksPerYear;
    int nFirstMPoSBlock;
    int nMPoSRewardRecipients;
    int nFixUTXOCacheHFHeight;
    int nEnableHeaderSignatureHeight;
    int nCheckpointSpan;
    int nRBTCheckpointSpan;
    uint160 delegationsAddress;
    uint160 delegationsAddressGasFix;
    uint160 lydraAddress;
    int nLastMPoSBlock;
    uint32_t nStakeTimestampMask;
    uint32_t nRBTStakeTimestampMask;
    int64_t nBlocktimeDownscaleFactor;
    /** Coinbase transaction outputs can only be spent after this number of new blocks (network rule) */
    int nCoinbaseMaturity;
    int nRBTCoinbaseMaturity;
    int64_t DifficultyAdjustmentInterval(int height, bool ignore) const
    {
        int64_t targetTimespan = TargetTimespan(height, ignore);
        int64_t targetSpacing = TargetSpacing(height);
        return targetTimespan / targetSpacing;
    }
    int64_t StakeTimestampMask(int height) const
    {
        return height < nReduceBlocktimeHeight ? nStakeTimestampMask : nRBTStakeTimestampMask;
    }
    int64_t BlocktimeDownscaleFactor(int height) const
    {
        return height < nReduceBlocktimeHeight ? 1 : nBlocktimeDownscaleFactor;
    }
    int64_t TargetSpacing(int height) const
    {
        return height < nReduceBlocktimeHeight ? nPowTargetSpacing : nRBTPowTargetSpacing;
    }
    int64_t TimestampDownscaleFactor(int height) const
    {
        return height < nReduceBlocktimeHeight ? 1 : (nStakeTimestampMask + 1) / (nRBTStakeTimestampMask + 1);
    }
    int64_t TargetTimespan(int height, bool ignore) const
    {
        if (height < QIP9Height)
        {
            return nPowTargetTimespan;
        }
        else if (height >= QIP9Height && height < nReduceBlocktimeHeight)
        {
            if (ignore) 
            {
                return nPowTargetTimespan;
            }
            else
            {
                return nPowTargetTimespanV2;
            }
        }
        else
        {
            if (ignore)
            {
                return nRBTPowTargetTimespan;
            }
            else
            {
                return nRBTPowTargetTimespanV2;
            }
        }
    }
    int CheckpointSpan(int height) const
    {
        return height < nReduceBlocktimeHeight ? nCheckpointSpan : nRBTCheckpointSpan;
    }
    int CoinbaseMaturity(int height) const
    {
        return height < nReduceBlocktimeHeight ? nCoinbaseMaturity : nRBTCoinbaseMaturity;
    }
    int MaxCheckpointSpan() const
    {
        return nCheckpointSpan <= nRBTCheckpointSpan ? nRBTCheckpointSpan : nCheckpointSpan;
    }
    uint160 GetDelegationsAddress(int height) const
    {
        return height < nDelegationsGasFixHeight ? delegationsAddress : delegationsAddressGasFix;
    }
};
} // namespace Consensus

#endif // BITCOIN_CONSENSUS_PARAMS_H
