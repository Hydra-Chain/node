// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>

#include <chainparamsseeds.h>
#include <consensus/merkle.h>
#include <consensus/consensus.h>
#include <pow.h>

#include <tinyformat.h>
#include <util/system.h>
#include <util/strencodings.h>
#include <util/convert.h>
#include <versionbitsinfo.h>

#include <assert.h>
#include <iostream>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <amount.h>
///////////////////////////////////////////// // qtum
#include <libdevcore/SHA3.h>
#include <libdevcore/RLP.h>
#include "arith_uint256.h"
#include "chainparams.h"

/////////////////////////////////////////////

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.nVersion = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 00 << 488804799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    genesis.hashStateRoot = uint256(h256Touint(dev::h256("59ad7784049dd6301cf2a839dd025acd3636b633bfc470990badec458366927d"))); // qtum
    genesis.hashUTXORoot = uint256(h256Touint(dev::sha3(dev::rlp("")))); // qtum
    return genesis;
}

/**
 * Build the genesis block. Note that the output of its generation
 * transaction cannot be spent since it did not originally exist in the
 * database.
 *
 * CBlock(hash=000000000019d6, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=4a5e1e, nTime=1231006505, nBits=1d00ffff, nNonce=2083236893, vtx=1)
 *   CTransaction(hash=4a5e1e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73)
 *     CTxOut(nValue=50.00000000, scriptPubKey=0x5F1DF16B2B704C8A578D0B)
 *   vMerkleTree: 4a5e1e
 */
static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "Sep 29, 2018 LockTrip Publishes its own Blockchain Manifest – And it is Amazing";
    const CScript genesisOutputScript = CScript() << ParseHex("033a537fcd935fba9f532ef78bb97154b5ea7b8dfd8b74facc36f7193e9d5b1cab") << OP_CHECKSIG;

    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

CChainParams::CChainParams()
{
    consensus.totalCoinsSupply = 1858593274150085; // supply is practically infinite
    consensus.initialCoinsSupply = consensus.totalCoinsSupply;
    consensus.blocksPerYear = 246375;
    consensus.nBlockRewardChangeInterval = 0xffffffffffffffff; // block reward changes with dgp
    consensus.BIP34Height = 0;
    consensus.BIP65Height = 0;
    consensus.BIP66Height = 0;
    consensus.QIP5Height = 0x7fffffff;
    consensus.QIP6Height = 0;
    consensus.QIP7Height = 0;
    consensus.QIP9Height = 5500;
    consensus.nFixUTXOCacheHFHeight=0;
    consensus.nEnableHeaderSignatureHeight = 5500;
    consensus.powLimit = uint256S("0000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    consensus.posLimit = uint256S("00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    consensus.QIP9PosLimit = uint256S("0000000000001fffffffffffffffffffffffffffffffffffffffffffffffffff"); // The new POS-limit activated after QIP9
    consensus.nPowTargetTimespan = 16 * 60; // 16 minutes
    consensus.nPowTargetTimespanV2 = 4000; // 5.59 hours
    consensus.nPowTargetSpacing = 2 * 64;
    consensus.fPowNoRetargeting = true;
    consensus.fPoSNoRetargeting = false;
    consensus.fPowAllowMinDifficultyBlocks = false;
    consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
    consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
    consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999; // December 31, 2008

    // Deployment of BIP68, BIP112, and BIP113.
    consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
    consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 0;
    consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 999999999999ULL;

    // Deployment of SegWit (BIP141, BIP143, and BIP147)
    consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].bit = 1;
    consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nStartTime = 0;
    consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout = 999999999999ULL;

    // The best chain should have at least this much work.
    consensus.nMinimumChainWork = uint256S("0x0000000000000000000000000000000000000000000000000000000000000001"); // qtum


    chainTxData = ChainTxData{
            // Data as of block 8f2ca6785f76fed5de90371c9c28eae941eaf24efb26a3641e2e276f3faca80e (height 17969)
            1582048400, // * UNIX timestamp of last known number of transactions
            1939818, // * total number of transactions between genesis and that timestamp
            //   (the tx=... number in the SetBestChain debug.log lines)
            1.17  // * estimated number of transactions per second after that timestamp
    };

    consensus.nLastPOWBlock = 5000;
    consensus.nMPoSRewardRecipients = 1;
    consensus.nFirstMPoSBlock = consensus.nLastPOWBlock +
                                consensus.nMPoSRewardRecipients +
                                COINBASE_MATURITY;
}

/**
 * Main network
 */
/**
 * What makes a good checkpoint block?
 * + Is surrounded by blocks with reasonable timestamps
 *   (no blocks before with a timestamp after, none after with
 *    timestamp before)
 * + Contains no strange transactions
 */

class CMainParams : public CChainParams {
public:
    CMainParams() {
        strNetworkID = "main";

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0xaf; // 175
        pchMessageStart[1] = 0xea; // 234
        pchMessageStart[2] = 0xe9; //233
        pchMessageStart[3] = 0xf7; //247
        nDefaultPort = 3338;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 6;
        m_assumed_chain_state_size = 2;

        genesis = CreateGenesisBlock(1535988275, 8201641, 0x1f00ffff, 1, 0 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x0000680af5389365703eeb6e4f26f8913d9d8b87344a38b06030a06c23bac26a"));
        assert(genesis.hashMerkleRoot == uint256S("0xbc4480addd2d1c0bf7ff88574831c52cd472c7f1caf1427d082b4e974748e8eb"));

		bech32_hrp = "qc";
        vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_main, pnSeed6_main + ARRAYLEN(pnSeed6_main));

        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        fMineBlocksOnDemand = false;

        checkpointData = (CCheckpointData) {
            {
                { 0, uint256S("0x0000680af5389365703eeb6e4f26f8913d9d8b87344a38b06030a06c23bac26a")},
            }
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,48);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,63);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,128);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        consensus.BIP34Hash = uint256S("0x0000680af5389365703eeb6e4f26f8913d9d8b87344a38b06030a06c23bac26a");
        // consensus.BIP65Height: 000000000000000004c2b624ed5d7756c508d90fd0da2c7c679febfa6c4735f0
		/* disable fallback fee on mainnet */
        m_fallback_fee_enabled = false;
        // consensus.BIP66Height: 00000000000000000379eaa19dce8c9b722d46ae6a57c2f1a988119488b50931

        consensus.nRuleChangeActivationThreshold = 1916; // 95% of 2016
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing

        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0xbfbbfc2c3be3d4e085082aff2e4e73a4e21dbf6205bc41b84b38ffac0a8bc114"); //453354
    }
};

/**
 * Testnet (v3)
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams() {
        strNetworkID = "test";

        pchMessageStart[0] = 0x07; // 7
        pchMessageStart[1] = 0x13; // 19
        pchMessageStart[2] = 0x1f; // 31
        pchMessageStart[3] = 0x03; // 3
        nDefaultPort = 1334;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 6;
        m_assumed_chain_state_size = 2;

        genesis = CreateGenesisBlock(1535988275, 7434208, 0x1f00ffff, 1, 0 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x0000f4e286ec9ad779b3ac6d22cdd1bd751c3a76a4c9ae6ccb7810cd110c3476"));
        assert(genesis.hashMerkleRoot == uint256S("0xbc4480addd2d1c0bf7ff88574831c52cd472c7f1caf1427d082b4e974748e8eb"));

        vFixedSeeds.clear();
        vSeeds.clear();
        vSeeds.push_back("devnet.locktrip.com");
        vSeeds.push_back("testnet.locktrip.com");
        vSeeds.push_back("testnet2.locktrip.com");
		bech32_hrp = "tq";

        vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_test, pnSeed6_test + ARRAYLEN(pnSeed6_test));

        fDefaultConsistencyChecks = false;
        fRequireStandard = false;
        fMineBlocksOnDemand = false;


        checkpointData = (CCheckpointData) {
            {
                {0, uint256S("0x0000f4e286ec9ad779b3ac6d22cdd1bd751c3a76a4c9ae6ccb7810cd110c3476")}
            }
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,66);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,128);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        // consensus.BIP65Height - 00000000007f6655f22f98e72ed80d8b06dc761d5da09df0fa1dc4be4f861eb6
        // consensus.BIP66Height - 000000002104c8c45e99a8853285a3b592602a3ccde2b832481da85e9e4ba182
        consensus.BIP34Hash = uint256S("0x0000f4e286ec9ad779b3ac6d22cdd1bd751c3a76a4c9ae6ccb7810cd110c3476");
		/* enable fallback fee on testnet */
        m_fallback_fee_enabled = true;
        consensus.posLimit = uint256S("0000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

        consensus.nRuleChangeActivationThreshold = 1512; // 75% for testchains
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing

        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0x39ffa0c5924550db0e75030ff8513c3145d491dff2e17b8e3ea1cea7b4662ff0"); //1079274
    }
};

/**
 * Regression test
 */
class CRegTestParams : public CChainParams {
public:
    CRegTestParams() {

        strNetworkID = "regtest";

        pchMessageStart[0] = 0xc7; // 199
        pchMessageStart[1] = 0xe8; // 232
        pchMessageStart[2] = 0xe2; // 226
        pchMessageStart[3] = 0x9f; // 159
        nDefaultPort = 2338;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        genesis = CreateGenesisBlock(1535988275, 18, 0x207fffff, 1, 0 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x3da51e432f82b0a829d0bda8d801932956495ac620e2ffb304d35164cf13663f"));
        assert(genesis.hashMerkleRoot == uint256S("0xbc4480addd2d1c0bf7ff88574831c52cd472c7f1caf1427d082b4e974748e8eb"));

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();      //!< Regtest mode doesn't have any DNS seeds.

        fDefaultConsistencyChecks = true;
        fRequireStandard = false;
        fMineBlocksOnDemand = true;

        checkpointData = (CCheckpointData) {
            {
                {0, uint256S("0x3da51e432f82b0a829d0bda8d801932956495ac620e2ffb304d35164cf13663f")},
            }
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,120);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,110);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};
		bech32_hrp = "qcrt";
        consensus.BIP34Hash = uint256S("0x3da51e432f82b0a829d0bda8d801932956495ac620e2ffb304d35164cf13663f");
        // consensus.BIP65Height:BIP65 activated on regtest (Used in rpc activation tests)
        // consensus.BIP66Height:BIP66 activated on regtest (Used in rpc activation tests)
        consensus.powLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
		/* enable fallback fee on regtest */
        m_fallback_fee_enabled = true;
        consensus.nMinimumChainWork = uint256S("0x00");

        consensus.nRuleChangeActivationThreshold = 108; // 75% for testchains
        consensus.nMinerConfirmationWindow = 144; // Faster than normal for regtest (144 instead of 2016)
        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0x00");
    }

};

/**
 * Regression network parameters overwrites for unit testing
 */
class CUnitTestParams : public CRegTestParams
{
public:
    CUnitTestParams()
    {
        // Activate the the BIPs for regtest as in Bitcoin
        consensus.BIP16Exception = uint256();
        consensus.BIP34Height = 100000000; // BIP34 has not activated on regtest (far in the future so block v1 are not rejected in tests)
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1351; // BIP65 activated on regtest (Used in rpc activation tests)
        consensus.BIP66Height = 1251; // BIP66 activated on regtest (Used in rpc activation tests)

        // LT have 500 blocks of maturity, increased values for regtest in unit tests in order to correspond with it
        consensus.blocksPerYear = 246375;
        consensus.nBlockRewardChangeInterval = 0xffffffffffffffff; // block reward changes with dgp
        consensus.totalCoinsSupply = 1858593274150085 ; // locktrip contract coin suply
        consensus.nRuleChangeActivationThreshold = 558; // 75% for testchains
        consensus.nMinerConfirmationWindow = 744; // Faster than normal for regtest (744 instead of 2016)
    }
};

static std::unique_ptr<CChainParams> globalChainParams;

CChainParams &Params() {
    assert(globalChainParams);
    return *globalChainParams;
}

std::unique_ptr<CChainParams> CreateChainParams(const std::string& chain)
{
    if (chain == CBaseChainParams::MAIN)
        return std::unique_ptr<CChainParams>(new CMainParams());
    else if (chain == CBaseChainParams::TESTNET)
        return std::unique_ptr<CChainParams>(new CTestNetParams());
    else if (chain == CBaseChainParams::REGTEST)
        return std::unique_ptr<CChainParams>(new CRegTestParams());
    else if (chain == CBaseChainParams::UNITTEST)
        return std::unique_ptr<CChainParams>(new CUnitTestParams());
    throw std::runtime_error(strprintf("%s: Unknown chain %s.", __func__, chain));
}

void SelectParams(const std::string& network)
{
    SelectBaseParams(network);
    globalChainParams = CreateChainParams(network);

//    std::unique_ptr<CChainParams> testChainParams = std::unique_ptr<CChainParams>(new CMainParams());
//
//    auto genesis = CreateGenesisBlock(1535988275, 8109746, 0x1f00ffff, 1, 0 * COIN);;
//
//    LogPrintf("Started working \n");
//    while (!CheckProofOfWork(genesis.GetHash(), genesis.nBits, ((CChainParams)*testChainParams).GetConsensus(), false)) {
//        ++genesis.nNonce;
//
//       // std::cout << "Working " << genesis.nNonce << "::";
//
//    }
//
//    std::cout << "Nonce " << genesis.nNonce << " Hash block " << genesis.GetHash().ToString() << std::endl;
//
//    LogPrintf("nNonce : %s \n", genesis.nNonce);
//
//    LogPrintf("Genesis : %s\n",genesis.GetHash().ToString());
//
//   testChainParams = std::unique_ptr<CChainParams>(new CTestNetParams());
//
//    genesis = CreateGenesisBlock(1535988275, 7359562, 0x1f00ffff, 1, 0 * COIN);
//
//    LogPrintf("Started working \n");
//    while (!CheckProofOfWork(genesis.GetHash(), genesis.nBits, ((CChainParams)*testChainParams).GetConsensus(), false)) {
//        ++genesis.nNonce;
//
//       // std::cout << "Working " << genesis.nNonce << "::";
//
//    }
//
//    std::cout << "Nonce " << genesis.nNonce << " Hash block " << genesis.GetHash().ToString()<< std::endl;
//
//    LogPrintf("nNonce : %s \n", genesis.nNonce);
//
//    LogPrintf("Genesis : %s\n",genesis.GetHash().ToString());
//
//    testChainParams = std::unique_ptr<CChainParams>(new CRegTestParams());
//
//    genesis = CreateGenesisBlock(1535988275, 18, 0x207fffff, 1, 0 * COIN);
//
//    LogPrintf("Started working \n");
//    while (!CheckProofOfWork(genesis.GetHash(), genesis.nBits, ((CChainParams)*testChainParams).GetConsensus(), false)) {
//        ++genesis.nNonce;
//
//     //   std::cout << "Working " << genesis.nNonce << "::";
//
//    }
//
//    std::cout << "Nonce " << genesis.nNonce << " Hash block " << genesis.GetHash().ToString()<< std::endl;
//
//    LogPrintf("nNonce : %s \n", genesis.nNonce);
//
//   LogPrintf("Genesis : %s\n",genesis.GetHash().ToString());
}

std::string CChainParams::EVMGenesisInfo(dev::eth::Network network) const
{
    std::string genesisInfo = dev::eth::genesisInfo(network);
    ReplaceInt(consensus.QIP7Height, "QIP7_STARTING_BLOCK", genesisInfo);
    ReplaceInt(consensus.QIP6Height, "QIP6_STARTING_BLOCK", genesisInfo);
    return genesisInfo;
}

std::string CChainParams::EVMGenesisInfo(dev::eth::Network network, int nHeight) const
{
    std::string genesisInfo = dev::eth::genesisInfo(network);
    ReplaceInt(nHeight, "QIP7_STARTING_BLOCK", genesisInfo);
    ReplaceInt(nHeight, "QIP6_STARTING_BLOCK", genesisInfo);
    return genesisInfo;
}
