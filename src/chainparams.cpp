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
    genesis.hashStateRoot = uint256(h256Touint(dev::h256("194423c6e17dd9a8fe0a5d44ba3f7217974242bac4317ec1d665d9a4a5a09700"))); // qtum
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
    const char* pszTimestamp = "Nov 01, 2020 Hydra Chain gets Approved — Work is in Full Swing";
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
    consensus.QIP6Height = 248000;
    consensus.QIP7Height = 0;
    consensus.QIP9Height = 5500;
    consensus.nFixUTXOCacheHFHeight=0;
    consensus.nEnableHeaderSignatureHeight = 5500;
    consensus.powLimit = uint256S("0000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    consensus.posLimit = uint256S("00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    consensus.QIP9PosLimit = uint256S("0000000000001fffffffffffffffffffffffffffffffffffffffffffffffffff"); // The new POS-limit activated after QIP9
    consensus.RBTPosLimit = uint256S("0000000000003fffffffffffffffffffffffffffffffffffffffffffffffffff");
    consensus.nPowTargetTimespan = 16 * 60; // 16 minutes
    consensus.nPowTargetTimespanV2 = 4000; // 5.59 hours
    consensus.nRBTPowTargetTimespan = 4 * 60;
    consensus.nRBTPowTargetTimespanV2 = 1000;
    consensus.nPowTargetSpacing = 2 * 64;
    consensus.nRBTPowTargetSpacing = 32;
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
    consensus.nMinimumChainWork = uint256S("0x00000000000000000000000000000000000000000000000000000000138b138b"); // qtum


    chainTxData = ChainTxData{
            // Data as of block c025e5c8d1b4991a6edc4a3f85539eacba2f321bfccad5f77abc6f1f3b472d01 (height 1176990)
            1689254868, // * UNIX timestamp of last known number of transactions
            2572776, // * total number of transactions between genesis and that timestamp
            //   (the tx=... number in the SetBestChain debug.log lines)
            0.031  // * estimated number of transactions per second after that timestamp
    };

    consensus.nLastPOWBlock = 5000;
    consensus.nMPoSRewardRecipients = 1;
    consensus.nCoinbaseMaturity = 500;
    consensus.nBlocktimeDownscaleFactor = 4;
    consensus.nRBTCoinbaseMaturity = consensus.nBlocktimeDownscaleFactor*500;
    consensus.nCheckpointSpan = consensus.nCoinbaseMaturity;
    consensus.nRBTCheckpointSpan = consensus.nRBTCoinbaseMaturity;
    consensus.nFirstMPoSBlock = consensus.nLastPOWBlock +
                                consensus.nMPoSRewardRecipients +
                                consensus.nCoinbaseMaturity;

    consensus.delegationsAddress = uint160(ParseHex("0000000000000000000000000000000000000093")); // Delegations contract for offline staking
    consensus.delegationsAddressGasFix = uint160(ParseHex("0000000000000000000000000000000000000094")); // Delegations contract for offline staking
    consensus.nStakeTimestampMask = 15;
    consensus.nRBTStakeTimestampMask = 3;
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

        genesis = CreateGenesisBlock(1535988275, 8121639, 0x1f00ffff, 1, 0 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x000058b8d49cd33ae70558978ff60269d4de7d4b50ac1f733631765e4207a457"));
        assert(genesis.hashMerkleRoot == uint256S("0x6b48eb829f7060b41346ba1f63486592c31dc04165f4c3fef8157943bd29b5cb"));

		bech32_hrp = "hc";
        vFixedSeeds.clear();
        vSeeds.clear();
        vSeeds.push_back("mainnetseeder.hydrachain.org");

        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        fMineBlocksOnDemand = false;

        checkpointData = (CCheckpointData) {
            {
                { 0, uint256S("0x000058b8d49cd33ae70558978ff60269d4de7d4b50ac1f733631765e4207a457")},
                { 5000, uint256S("0x0000a93f9349490201a1877db655f010415874fb2be833565c7f96984e8b046c")},
                { 50000, uint256S("0x7e44ec40710d819a259d32aba0a57365e6c939fa82f3d469f843e8e0c831589b")},
                { 100000, uint256S("0x9f61651dcf61582f730c3b45930247a56b691cc6070bcf3d5c5a58f4e5b5c575")},
                { 150000, uint256S("0xaad4409b6728f09c3b7927776a5473e569a11e019c12e1d613ef9fe46ce8f018")},
                { 200000, uint256S("0x65d6d7a3cc3b87ae4ae46297d0d371f3056db94265ba4f98da8530f5f6bd8f50")},
                { 248000, uint256S("0x2757d9bc6a7764f84eaf006c7f7f43b653b11c93e8f709b83300648cbfca251a")},
                { 300000, uint256S("0x4dc449e81e83d2545b1fd667b1e7b7cde13f88fab886251b1afb3356f0fb1737")},
                { 350000, uint256S("0x4cb2fe2dea4285fae07ee7422cf845a6792c50b19c44be9d09ea19b9b43a6d0f")},
                { 400000, uint256S("0xac0a9efae375664bb7e69a9058d0131682814f9ef81ebe12f40e9b03efbb817c")},
                { 450000, uint256S("0xe05c213e3c098e5f5dbfdf8e7da5964483af82831c978a042554fedb86ce5ffc")},
                { 477300, uint256S("0xfadf1901b5a9c5acedee65e214e50bd122a6217389d5006370ac82089854dccf")},
                { 500000, uint256S("0x5856cfd4b848304cac9464c2f52666e64968a1d478ec03d6d54d0afe34db5fcd")},
                { 550000, uint256S("0x74703000a2392360686709f66b81e3653542e3b2afe49cce1e712092d7401b8d")},
                { 600000, uint256S("0x43923b408cd5061af5b8393f8f3f18b783a700592bf089bc661eca943d6c6fc4")},
                { 650000, uint256S("0x9e5ce6df05fd7f901266e2fc3becdcb220f8c6f565a94a9d788a5ac909a2965e")},
                { 658600, uint256S("0xfcf727239322c8023cdff57dacc20b4cd22530d979f4e18661562edd9e30751c")},
                { 700000, uint256S("0xf3281e8cc00c8010929a270d47dc124d4ff1afb532a3aea5f15a6bdd282f00b8")},
                { 750000, uint256S("0x082b8c7c90a077365eee2fa5b3cb7cac8f31051ff6daf597e63d73a88390c853")},
                { 800000, uint256S("0x5b4ead3de1ac2624703799734e7bafaf3370a061a0eac184ab785ba00012e97b")},
                { 850000, uint256S("0xd61afa26c97b32559e7c9374c29bb2976b88618d7f5880f1b402a6ec580e0ad6")},
                { 852800, uint256S("0x08dbeaccf351c68a8a04b67e49a8189a548a9e2ebcc7adfdfaaaff26ad425ca7")},
                { 900000, uint256S("0x3fee8aaac367236a34191b0f36ff675920ba18be84bb71278441fe39d9813f4c")},
                { 1160399, uint256S("0xaab765a09315f73b9e23a7a1aa072635ffacdd0844b1221645ac1bd5c89180b0")},
                { 1160400, uint256S("0xfc13ccffed1470787816b0716f1be963a9f5a4d3f6c6beb392c1d455d450eb59")},
                { 1161400, uint256S("0x39b9e68f75ebb20d70985707e9a3f59475824db241d8971fba9630e865c4528f")},
                { 1162400, uint256S("0x1289615b90376e62051c64a09424d370ab33eb7405e72445cd35343d7c0e5386")},
                { 1163400, uint256S("0xc77aa02846a8c46f56e27fce03a3d1914dc341fbc752009c00b356b8cf414839")},
                { 1164400, uint256S("0x6f76bd57004f647202a2f5defefebae19b1d4ae605c50c99aece7035b80a4e42")},
                { 1165400, uint256S("0x007270a040d4a04ffde433fbb38e6b05b54928a098c9b5165ee0e423f608ae1f")},
                { 1166400, uint256S("0xcad66f377f7c5adf450f055d5e5905755f0676046010d30e355e2d2a906b7e60")},
                { 1167400, uint256S("0xdae1ac92dcd56eaf333bad08a19ba949a995061f20a5b895fe36274780936c98")},
                { 1168400, uint256S("0x0018bf41fc6ddf962470ee2e9d8cecea481c671d29fe87b061216a6705e310f3")},
                { 1169400, uint256S("0x8cd15f5d6efe7c19b920bd577d587d38991cf3147ed43621909d30a758fa2025")},
                { 1170400, uint256S("0x850a47abd7ae43307e8c0280c507cc1b8a0037f73300f202426e7d2a04135a44")},
                { 1171400, uint256S("0x1553338a3be271c74ac265d74c1650924d1bc8d54a6a87c0fdfd892f12f1c211")},
                { 1172400, uint256S("0x163d842f21873ccaa095bc13df914fd35a3ded75e5865ee4241b80eb9ae3d9f1")},
                { 1173400, uint256S("0xa3efce07234e63a42350bdf0379fa9bf4abeb983391cc0b4c68f30afd343c285")},
                { 1174400, uint256S("0x84ea50f1c988d9bf2253a7f6c5583eb6183c0daf0255c19c536fdc770ecd60de")},
                { 1175400, uint256S("0xf7125e3c1d9a277da05c74f16fa6d7055138b6ffa81f33c59f4336e4462687f1")},
                { 1176400, uint256S("0x92660e79da8f13d05ec97a4ecede44aa66d379b4592feebe9fbea769435de17b")},
                { 1200000, uint256S("0x6617a571de572c2f50309069ae3ccec37bf42d987f746ee58b6e983649db3d57")},
                { 1300000, uint256S("0xac7cd4cc1b0df660e7e6b314ab2580304d514e3acf428da01c419f2f193654f1")},
                { 1400000, uint256S("0x929c878da05dfaaea9a5c02f9eac4b3f1f870b10fd51d11072675e68de68e72c")},
                { 1500000, uint256S("0x62ecaaa9e481ed01303c1ee78ff7205e5b45c00719646b304bae054978cf194a")},
                { 1600000, uint256S("0x1ed6cb9a4862c373f0f7dbf07b83f1e890214be4040683427ecb620905438c14")},
                { 1700000, uint256S("0xbbffaaf329861df74e38cb484314265e6335347f261601620aeae5e1eebd1b8a")},
                { 1800000, uint256S("0x7b03baeaa64b37fee3dec45990386389947a502911925add5520f7fde2cb8067")},
            }
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,40);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,63);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,128);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        consensus.MuirGlacierHeight = 477300;
        consensus.nOfflineStakeHeight = 477300;
        consensus.nReduceBlocktimeHeight = 477300;
        consensus.nLastMPoSBlock = 477299;
        consensus.nRewardFixHeight = 658600;
        consensus.nRewardOffsetHeight = 658600;
        consensus.nRewardOffsetAmount = 33018156505135300;
        consensus.nContractOutsHeight = 852800;
        consensus.nRefundFixHeight = 1160400;
        consensus.nLydraHeight = 1160400;
        consensus.lydraAddress = uint160(ParseHex("68a674e10694eeb00867d2e11c8b158b00b28e96")); // LYDRA token address
        consensus.nDelegationsGasFixHeight = 1160400;
        consensus.nLydraOverspendingFixHeight = 1517000;
        consensus.nStakerOptimizationHeight = 1849150;

        consensus.BIP34Hash = uint256S("0x000058b8d49cd33ae70558978ff60269d4de7d4b50ac1f733631765e4207a457");
        // consensus.BIP65Height: 000000000000000004c2b624ed5d7756c508d90fd0da2c7c679febfa6c4735f0
		/* disable fallback fee on mainnet */
        m_fallback_fee_enabled = false;
        // consensus.BIP66Height: 00000000000000000379eaa19dce8c9b722d46ae6a57c2f1a988119488b50931

        consensus.nRuleChangeActivationThreshold = 1916; // 95% of 2016
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing

        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0x0x92660e79da8f13d05ec97a4ecede44aa66d379b4592feebe9fbea769435de17b"); //1176400
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
        nDefaultPort = 13333;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 6;
        m_assumed_chain_state_size = 2;

        genesis = CreateGenesisBlock(1535988275, 7555354, 0x1f00ffff, 1, 0 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x00002670c621a105d887fec054bead4805e4f0f8661802d71cec52f5d71c76d8"));
        assert(genesis.hashMerkleRoot == uint256S("0x6b48eb829f7060b41346ba1f63486592c31dc04165f4c3fef8157943bd29b5cb"));

        vFixedSeeds.clear();
        vSeeds.clear();
        vSeeds.push_back("testnetseeder.hydrachain.org");
		bech32_hrp = "th";

        vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_test, pnSeed6_test + ARRAYLEN(pnSeed6_test));

        fDefaultConsistencyChecks = false;
        fRequireStandard = false;
        fMineBlocksOnDemand = false;


        checkpointData = (CCheckpointData) {
            {
                {0, uint256S("0x00002670c621a105d887fec054bead4805e4f0f8661802d71cec52f5d71c76d8")},
		        { 5000, uint256S("0x00002327c0c0f1ce9cd3ae49c4d4aa4f63c7d9b05442653ed4eee2443cb00ed8")},
                { 50000, uint256S("0x0fe80e66280bc80112dde1ee4b3c6f2bf7025d6f7158ec0950b99bdfcd7f1177")},
                { 100000, uint256S("0x08ef0b6bfa457ba180165b5d7810df46f6b4b4c7a3b90ed9bef35aa495695cfd")},
                { 150000, uint256S("0x08260c1b6cedd2098f83d12d1740c951539ad1242c26a40ffc10d71a04ba3a09")},
                { 200000, uint256S("0x73d9f563976cbd2ca226427c75af81dbd1c25b4b2c2e909bc57ba7ba480e1ea9")},
                { 248000, uint256S("0x18675de3fbb6043de88f6a187bdbb60f4b1f46d267d9807bded16c22bbfd2e23")},
		        { 270200, uint256S("0x6204efcab8a85c6dbb29997787a3fc1276257c8e83ba9783dd93aeb9bb47a802")},
                { 300000, uint256S("0x78bd24c4f7f6b08bf5817fa400823460188aa700e4d0f7ea84f368bcdf2beaa0")},
                { 350000, uint256S("0x7b41832162e265ee5be8e487d23ce2445493e2fc296a4d7407c39af01ec02ece")},
                { 400000, uint256S("0x4a8d9d3a902e2d75d3bb74f28cd9f5b70e848ab8f05aeb1d94ef38a8f07d7c10")},
                { 450000, uint256S("0x82a07944b03820de2694c90d730b55fac5f8ed860bb7c251715257bba111e8f1")},
                { 500000, uint256S("0x1565a6317500662cc897c8f209490ab50b28dac809fc6ea54efac86c74b06675")},
                { 550000, uint256S("0x7d3636fe7aa16c0f354a3b47052123887360b774d5e89e4858fd9e253c657e3c")},
                { 600000, uint256S("0xd02a53003c5e0f560ec0e4277659d05f033e6f00b341f1a556b51e40a0875a41")},
                { 650000, uint256S("0x53054a85846bd61a8ac2e408e4e55f3693d11f59baee6f7997ac2bf1537907ce")},
                { 700000, uint256S("0xa45a7ecb588816f8f5b334f1679de4022b588aedc8a0b8e3489d9d695db2de27")},
                { 750000, uint256S("0x9a75e1040e42fab8001943110e924253acc43a9350431bd8f9958287d71c1bd2")},
                { 785140, uint256S("0xf5856a573033b5a9ce16344ae2d8094ad619d16a1fcc84698abe45f3fdb96e9c")},
                { 800000, uint256S("0x180920ee76d8aba4bef43de8d3f3aa22c966c32ac92ee7b860f6937d003bd3bc")},
                { 850000, uint256S("0x9a93c5af1e3c2990c0870e045ce3fb373278302ca7f4857842608fe5c7648fbb")},
                { 900000, uint256S("0xab00f546b5e8805a06ac9e109f358c47585ac63a07568145b7930e2bde40412f")},
                { 950000, uint256S("0x5b5012f2262fcbcd9f769eecdf57cde6dd22e1d6a405f36a89230a2fa8b60da5")},
                { 979410, uint256S("0x282cdf3aa4957a50b6cac6e38f426a9c148cfb710084cf733575396ee08142b4")},
                { 1000000, uint256S("0x7d73d44f39f5b699a4066f7ab9e36297dcef49e14e37524d6f50a4805ad06acc")},
                { 1050000, uint256S("0x3f9e68d1bea7ad90511ab5279b28561920acd6ebb374bddc17ec8f86029b0c71")},
                { 1150000, uint256S("0xefec909ac85edda4dcdf849f81dbb3d2497b2f35a38f80281b352065c080b9a1")},
                { 1250000, uint256S("0x8736de5c7e7bf3978f8573cc77e50293736430cbb878a230f49668ab13b75809")},
                { 1350000, uint256S("0x99c0973f3895f7727a5939aa1d9365e9de3474412ba3d75d04b8f69a916b7222")},
                { 1450000, uint256S("0xd5b008d3676f836683812bc84713943805d223aa2615f22f27c2c83461b55168")},
                { 1550000, uint256S("0x62786bb5005da5146baf07affeedf3c7b4bfc43ab3b77a92b21ece836dac0f52")},
                { 1650000, uint256S("0x7e96481b9f4341713dd6c674d7617e043a185d32f769231ab045e7773fc1da1c")},
                { 1750000, uint256S("0xb0ebaaa21743d4b3a6d902492149b8fd494cf514db46d0aa07eeb94258ad3495")},
                { 1850000, uint256S("0x774a56770e9cdcd0d26cd91a5a5c9b7a741a043ad2e2fc50160c7efaf4f958a0")},
            }
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,66);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,128);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        consensus.MuirGlacierHeight = 270200;
        consensus.nOfflineStakeHeight = 270200;
        consensus.nReduceBlocktimeHeight = 270200;
        consensus.nLastMPoSBlock = 270199;
        consensus.nRewardFixHeight = 785140;
        consensus.nRewardOffsetHeight = 785140;
        consensus.nRewardOffsetAmount = 94225990557823100;
        consensus.nContractOutsHeight = 979410;
        consensus.nLydraHeight = 1071300;
        consensus.lydraAddress = uint160(ParseHex("438d420c1cadefab544c5d2b9adf62c380b40934")); // LYDRA token address
        consensus.nDelegationsGasFixHeight = 1071300;
        consensus.nRefundFixHeight = 1153800;
        consensus.nLydraOverspendingFixHeight = 1539720;
        consensus.nStakerOptimizationHeight = 1934500;

        // consensus.BIP65Height - 00000000007f6655f22f98e72ed80d8b06dc761d5da09df0fa1dc4be4f861eb6
        // consensus.BIP66Height - 000000002104c8c45e99a8853285a3b592602a3ccde2b832481da85e9e4ba182
        consensus.BIP34Hash = uint256S("0x00002670c621a105d887fec054bead4805e4f0f8661802d71cec52f5d71c76d8");
		/* enable fallback fee on testnet */
        m_fallback_fee_enabled = true;
        consensus.posLimit = uint256S("0000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

        consensus.nRuleChangeActivationThreshold = 1512; // 75% for testchains
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing

        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0x3f9e68d1bea7ad90511ab5279b28561920acd6ebb374bddc17ec8f86029b0c71"); //1050000
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
        assert(consensus.hashGenesisBlock == uint256S("0x5cd5683b9f18b677458483c7ef530dca2471c45ba240e4e57250962873aebda1"));
        assert(genesis.hashMerkleRoot == uint256S("0x6b48eb829f7060b41346ba1f63486592c31dc04165f4c3fef8157943bd29b5cb"));

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();      //!< Regtest mode doesn't have any DNS seeds.

        fDefaultConsistencyChecks = true;
        fRequireStandard = false;
        fMineBlocksOnDemand = true;

        checkpointData = (CCheckpointData) {
            {
                {0, uint256S("0x5cd5683b9f18b677458483c7ef530dca2471c45ba240e4e57250962873aebda1")},
            }
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,120);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,110);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};
		bech32_hrp = "qcrt";
        consensus.BIP34Hash = uint256S("0x5cd5683b9f18b677458483c7ef530dca2471c45ba240e4e57250962873aebda1");
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

        // HYDRA have 500 blocks of maturity, increased values for regtest in unit tests in order to correspond with it
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
//    std::cout << "merkle root " << genesis.GetBlockHeader().hashMerkleRoot.ToString() << std::endl;
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
//    std::cout << "merkle root " << genesis.GetBlockHeader().hashMerkleRoot.ToString() << std::endl;
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
//    std::cout << "merkle root " << genesis.GetBlockHeader().hashMerkleRoot.ToString() << std::endl;
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
