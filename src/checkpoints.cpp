// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <checkpoints.h>

#include <chain.h>
#include <chainparams.h>
#include <reverse_iterator.h>
#include <validation.h>

#include <stdint.h>

namespace Checkpoints {

    bool CheckHardened(int nHeight, const uint256& hash, const CCheckpointData& data)
    {
        const MapCheckpoints& checkpoints = data.mapCheckpoints;

        MapCheckpoints::const_iterator i = checkpoints.find(nHeight);
        if (i == checkpoints.end()) return true;
        return hash == i->second;
    }

    CBlockIndex* GetLastCheckpoint(const CCheckpointData& data)
    {
        const MapCheckpoints& checkpoints = data.mapCheckpoints;

        for (const MapCheckpoints::value_type& i : reverse_iterate(checkpoints))
        {
            const uint256& hash = i.second;
            CBlockIndex* pindex = LookupBlockIndex(hash);
            if (pindex) {
                return pindex;
            }
        }
        return nullptr;
    }

    // Automatically select a suitable sync-checkpoint 
    const CBlockIndex* AutoSelectSyncCheckpoint()
    {
        const CBlockIndex *pindexBest = chainActive.Tip();
        const CBlockIndex *pindex = pindexBest;
        // Search backward for a block within max span and maturity window
        int checkpointSpan = Params().GetConsensus().CheckpointSpan(pindexBest->nHeight);
        while (pindex->pprev && pindex->nHeight + checkpointSpan > pindexBest->nHeight)
            pindex = pindex->pprev;
        return pindex;
    }

    // Check against synchronized checkpoint
    bool CheckSync(int nHeight)
    {
        const CBlockIndex* pindexSync;
        if(nHeight)
            pindexSync = AutoSelectSyncCheckpoint();

        if(nHeight && nHeight <= pindexSync->nHeight)
            return false;
        return true;
    }
} // namespace Checkpoints
