// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include <coins.h>
#include <dbwrapper.h>
#include <chain.h>
#include <primitives/block.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <validation.h> // temp

class CBlockIndex;
class CCoinsViewDBCursor;
class uint256;

//////////////////////////////////// //qtum
struct CAddressIndexKey;
struct CAddressUnspentKey;
struct CAddressUnspentValue;
struct CMempoolAddressDeltaKey;
struct CTimestampIndexKey;
struct CTimestampBlockIndexKey;
struct CTimestampBlockIndexValue;
////////////////////////////////////

//! Compensate for extra memory peak (x1.5-x1.9) at flush time.
static constexpr int DB_PEAK_USAGE_FACTOR = 2;
//! No need to periodic flush if at least this much space still available.
static constexpr int MAX_BLOCK_COINSDB_USAGE = 10 * DB_PEAK_USAGE_FACTOR;
//! -dbcache default (MiB)
static const int64_t nDefaultDbCache = 450;
//! -dbbatchsize default (bytes)
static const int64_t nDefaultDbBatchSize = 16 << 20;
//! max. -dbcache (MiB)
static const int64_t nMaxDbCache = sizeof(void*) > 4 ? 16384 : 1024;
//! min. -dbcache (MiB)
static const int64_t nMinDbCache = 4;
//! Max memory allocated to block tree DB specific cache, if no -txindex (MiB)
static const int64_t nMaxBlockDBCache = 2;
//! Max memory allocated to block tree DB specific cache, if -txindex (MiB)
// Unlike for the UTXO database, for the txindex scenario the leveldb cache make
// a meaningful difference: https://github.com/bitcoin/bitcoin/pull/8273#issuecomment-229601991
static const int64_t nMaxTxIndexCache = 1024;
//! Max memory allocated to coin DB specific cache (MiB)
static const int64_t nMaxCoinsDBCache = 8;

struct CDiskTxPos : public CDiskBlockPos
{
    unsigned int nTxOffset; // after header

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITEAS(CDiskBlockPos, *this);
        READWRITE(VARINT(nTxOffset));
    }

    CDiskTxPos(const CDiskBlockPos &blockIn, unsigned int nTxOffsetIn) : CDiskBlockPos(blockIn.nFile, blockIn.nPos), nTxOffset(nTxOffsetIn) {
    }

    CDiskTxPos() {
        SetNull();
    }

    void SetNull() {
        CDiskBlockPos::SetNull();
        nTxOffset = 0;
    }
};

/** CCoinsView backed by the coin database (chainstate/) */
class CCoinsViewDB final : public CCoinsView
{
protected:
    CDBWrapper db;
public:
    explicit CCoinsViewDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    bool GetCoin(const COutPoint &outpoint, Coin &coin) const override;
    bool HaveCoin(const COutPoint &outpoint) const override;
    uint256 GetBestBlock() const override;
    std::vector<uint256> GetHeadBlocks() const override;
    bool BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock) override;
    CCoinsViewCursor *Cursor() const override;

    //! Attempt to update from an older database format. Returns whether an error occurred.
    bool Upgrade();
    size_t EstimateSize() const override;
};

/** Specialization of CCoinsViewCursor to iterate over a CCoinsViewDB */
class CCoinsViewDBCursor: public CCoinsViewCursor
{
public:
    ~CCoinsViewDBCursor() {}

    bool GetKey(COutPoint &key) const override;
    bool GetValue(Coin &coin) const override;
    unsigned int GetValueSize() const override;

    bool Valid() const override;
    void Next() override;

private:
    CCoinsViewDBCursor(CDBIterator* pcursorIn, const uint256 &hashBlockIn):
        CCoinsViewCursor(hashBlockIn), pcursor(pcursorIn) {}
    std::unique_ptr<CDBIterator> pcursor;
    std::pair<char, COutPoint> keyTmp;

    friend class CCoinsViewDB;
};

/** Access to the block database (blocks/index/) */
class CBlockTreeDB : public CDBWrapper
{
public:
    explicit CBlockTreeDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    bool WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo);
    bool ReadBlockFileInfo(int nFile, CBlockFileInfo &info);
    bool ReadLastBlockFile(int &nFile);
    bool WriteReindexing(bool fReindexing);
    void ReadReindexing(bool &fReindexing);
    bool WriteFlag(const std::string &name, bool fValue);
    bool ReadFlag(const std::string &name, bool &fValue);
    bool LoadBlockIndexGuts(const Consensus::Params& consensusParams, std::function<CBlockIndex*(const uint256&)> insertBlockIndex);

    ////////////////////////////////////////////////////////////////////////////// // qtum
    bool WriteHeightIndex(const CHeightTxIndexKey &heightIndex, const std::vector<uint256>& hash);

    /**
     * Iterates through blocks by height, starting from low.
     *
     * @param low start iterating from this block height
     * @param high end iterating at this block height (ignored if <= 0)
     * @param minconf stop iterating of the block height does not have enough confirmations (ignored if <= 0)
     * @param blocksOfHashes transaction hashes in blocks iterated are collected into this vector.
     * @param addresses filter out a block unless it matches one of the addresses in this set.
     *
     * @return the height of the latest block iterated. 0 if no block is iterated.
     */
    int ReadHeightIndex(int low, int high, int minconf,
            std::vector<std::vector<uint256>> &blocksOfHashes,
            std::set<dev::h160> const &addresses);
    bool EraseHeightIndex(const unsigned int &height);
    bool WipeHeightIndex();


    bool WriteStakeIndex(unsigned int height, uint160 address);
    bool ReadStakeIndex(unsigned int height, uint160& address);
    bool ReadStakeIndex(unsigned int high, unsigned int low, std::vector<uint160> addresses);
    bool EraseStakeIndex(unsigned int height);

    bool WriteDelegateIndex(unsigned int height, uint160 address, uint8_t fee);
    bool ReadDelegateIndex(unsigned int height, uint160& address, uint8_t& fee);
    bool EraseDelegateIndex(unsigned int height);

    // Block explorer database functions
    bool WriteAddressIndex(const std::vector<std::pair<CAddressIndexKey, CAmount> > &vect);
    bool EraseAddressIndex(const std::vector<std::pair<CAddressIndexKey, CAmount> > &vect);
    bool ReadAddressIndex(uint256 addressHash, int type,
                        std::vector<std::pair<CAddressIndexKey, CAmount> > &addressIndex,
                        int start = 0, int end = 0);
    bool UpdateAddressUnspentIndex(const std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue > >&vect);
    bool ReadAddressUnspentIndex(uint256 addressHash, int type,
                                std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > &vect);
    bool WriteTimestampIndex(const CTimestampIndexKey &timestampIndex);
    bool ReadTimestampIndex(const unsigned int &high, const unsigned int &low, const bool fActiveOnly, std::vector<std::pair<uint256, unsigned int> > &vect);
    bool WriteTimestampBlockIndex(const CTimestampBlockIndexKey &blockhashIndex, const CTimestampBlockIndexValue &logicalts);
    bool ReadTimestampBlockIndex(const uint256 &hash, unsigned int &logicalTS);
    bool ReadSpentIndex(CSpentIndexKey &key, CSpentIndexValue &value);
    bool UpdateSpentIndex(const std::vector<std::pair<CSpentIndexKey, CSpentIndexValue> >&vect);
    bool blockOnchainActive(const uint256 &hash);

    //////////////////////////////////////////////////////////////////////////////

};

#endif // BITCOIN_TXDB_H
