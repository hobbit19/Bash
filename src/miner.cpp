// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2013 The NovaCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdb.h"
#include "miner.h"
#include "kernel.h"
#include "bitbet.h"

using namespace std;

//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

extern unsigned int nMinerSleep;
extern bool bTimeSyncedFromNtpServer;
extern string strLocalPublicIP;
bool bMiner_SyncBlockChain = false,  bNormalMinerWeight=true;
uint64_t nEstimateHeight=0;

void setEstimateHeight(uint64_t newHeight)
{
    if( newHeight > nBestHeight )
    {
        nEstimateHeight = newHeight;
        //if(fDebug) printf("setEstimateHeight(%s)\n",u64tostr(nEstimateHeight).c_str());
    }
}
int static FormatHashBlocks(void* pbuffer, unsigned int len)
{
    unsigned char* pdata = (unsigned char*)pbuffer;
    unsigned int blocks = 1 + ((len + 8) / 64);
    unsigned char* pend = pdata + 64 * blocks;
    memset(pdata + len, 0, 64 * blocks - len);
    pdata[len] = 0x80;
    unsigned int bits = len * 8;
    pend[-1] = (bits >> 0) & 0xff;
    pend[-2] = (bits >> 8) & 0xff;
    pend[-3] = (bits >> 16) & 0xff;
    pend[-4] = (bits >> 24) & 0xff;
    return blocks;
}

static const unsigned int pSHA256InitState[8] =
{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

void SHA256Transform(void* pstate, void* pinput, const void* pinit)
{
    SHA256_CTX ctx;
    unsigned char data[64];

    SHA256_Init(&ctx);

    for (int i = 0; i < 16; i++)
        ((uint32_t*)data)[i] = ByteReverse(((uint32_t*)pinput)[i]);

    for (int i = 0; i < 8; i++)
        ctx.h[i] = ((uint32_t*)pinit)[i];

    SHA256_Update(&ctx, data, sizeof(data));
    for (int i = 0; i < 8; i++)
        ((uint32_t*)pstate)[i] = ctx.h[i];
}

// Some explaining would be appreciated
class COrphan
{
public:
    CTransaction* ptx;
    set<uint256> setDependsOn;
    double dPriority;
    double dFeePerKb;

    COrphan(CTransaction* ptxIn)
    {
        ptx = ptxIn;
        dPriority = dFeePerKb = 0;
    }

    void print() const
    {
        printf("COrphan(hash=%s, dPriority=%.1f, dFeePerKb=%.1f)\n",
               ptx->GetHash().ToString().substr(0,10).c_str(), dPriority, dFeePerKb);
        BOOST_FOREACH(uint256 hash, setDependsOn)
            printf("   setDependsOn %s\n", hash.ToString().substr(0,10).c_str());
    }
};


uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;
int64_t nLastCoinStakeSearchInterval = 0;
 
// We want to sort transactions by priority and fee, so:
typedef boost::tuple<double, double, CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;
public:
    TxPriorityCompare(bool _byFee) : byFee(_byFee) { }
    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if (byFee)
        {
            if (a.get<1>() == b.get<1>())
                return a.get<0>() < b.get<0>();
            return a.get<1>() < b.get<1>();
        }
        else
        {
            if (a.get<0>() == b.get<0>())
                return a.get<1>() < b.get<1>();
            return a.get<0>() < b.get<0>();
        }
    }
};

// CreateNewBlock: create new block (without proof-of-work/proof-of-stake)
CBlock* CreateNewBlock(CWallet* pwallet, bool fProofOfStake, int64_t* pFees)
{
    // Create new block
    auto_ptr<CBlock> pblock(new CBlock());
    if (!pblock.get())
        return NULL;

    CBlockIndex* pindexPrev = pindexBest;
    int64_t nHeight = pindexPrev->nHeight + 1;

    if (!IsProtocolV2(nHeight))
        pblock->nVersion = 6;

    bool bQPoS_Rules_Actived = Is_Queue_PoS_Rules_Acitved(nHeight);
    if( bQPoS_Rules_Actived )
	{
		if( Is_QPoS_Rules704_Actived(nHeight) )
		{
			if( !bSystemNodeWallet ){ pblock->blockData = strLocalPublicIP; }
		}
		else{ pblock->blockData = i64tostr(nHeight); }
	}

    // Create coinbase tx
    CTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);

    if (!fProofOfStake)
    {
        CReserveKey reservekey(pwallet);
        CPubKey pubkey;
        if (!reservekey.GetReservedKey(pubkey))
            return NULL;
        txNew.vout[0].scriptPubKey.SetDestination(pubkey.GetID());
    }
    else
    {
        // Height first in coinbase required for block.version=2
        txNew.vin[0].scriptSig = (CScript() << nHeight) + COINBASE_FLAGS;
        assert(txNew.vin[0].scriptSig.size() <= 100);

        txNew.vout[0].SetEmpty();
    }

    // Add our coinbase tx as first transaction
    pblock->vtx.push_back(txNew);

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", max_BLOCK_SIZE_GEN(nBestHeight));  //MAX_BLOCK_SIZE_GEN
    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(max_BLOCK_SIZE(nBestHeight)-1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", 27000);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", 0);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Fee-per-kilobyte amount considered the same as "free"
    // Be careful setting this: if you set it to zero then
    // a transaction spammer can cheaply fill blocks using
    // 1-satoshi-fee transactions. It should be set above the real
    // cost to you of processing a transaction.
    int64_t nMinTxFee = getMIN_TX_FEE(nBestHeight);
    if (mapArgs.count("-mintxfee"))
        ParseMoney(mapArgs["-mintxfee"], nMinTxFee);

    pblock->nBits = GetNextTargetRequired(pindexPrev, fProofOfStake);

    // Collect memory pool transactions into the block
    int64_t nFees = 0;
    {
        LOCK2(cs_main, mempool.cs);
        CTxDB txdb("r");

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());
        for (map<uint256, CTransaction>::iterator mi = mempool.mapTx.begin(); mi != mempool.mapTx.end(); ++mi)
        {
            CTransaction& tx = (*mi).second;
            if (tx.IsCoinBase() || tx.IsCoinStake() || !IsFinalTx(tx, nHeight))
                continue;

            COrphan* porphan = NULL;
            double dPriority = 0;
            int64_t nTotalIn = 0;
            bool fMissingInputs = false;
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
            {
                // Read prev transaction
                CTransaction txPrev;
                CTxIndex txindex;
                if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
                {
                    // This should never happen; all transactions in the memory
                    // pool should connect to either transactions in the chain
                    // or other transactions in the memory pool.
                    if (!mempool.mapTx.count(txin.prevout.hash))
                    {
                        printf("ERROR: mempool transaction missing input\n");
                        //if (fDebug) assert("mempool transaction missing input" == 0);
                        fMissingInputs = true;
                        if (porphan)
                            vOrphan.pop_back();
                        break;
                    }

                    // Has to wait for dependencies
                    if (!porphan)
                    {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }
                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    nTotalIn += mempool.mapTx[txin.prevout.hash].vout[txin.prevout.n].nValue;
                    continue;
                }
                int64_t nValueIn = txPrev.vout[txin.prevout.n].nValue;
                nTotalIn += nValueIn;

                int nConf = txindex.GetDepthInMainChain();
                dPriority += (double)nValueIn * nConf;
            }
            if (fMissingInputs) continue;

            // Priority is sum(valuein * age) / txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority /= nTxSize;

            // This is a more accurate fee-per-kilobyte than is used by the client code, because the
            // client code rounds up the size to the nearest 1K. That's good, because it gives an
            // incentive to create smaller transactions.
            double dFeePerKb =  double(nTotalIn-tx.GetValueOut()) / (double(nTxSize)/1000.0);

            if (porphan)
            {
                porphan->dPriority = dPriority;
                porphan->dFeePerKb = dFeePerKb;
            }
            else
                vecPriority.push_back(TxPriority(dPriority, dFeePerKb, &(*mi).second));
        }

        // Collect transactions into block
        map<uint256, CTxIndex> mapTestPool;
        uint64_t nBlockSize = 1000;
        uint64_t nBlockTx = 0;
        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty())
        {
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            double dFeePerKb = vecPriority.front().get<1>();
            CTransaction& tx = *(vecPriority.front().get<2>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize)
                continue;

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = tx.GetLegacySigOpCount();
            if (nBlockSigOps + nTxSigOps >= max_BLOCK_SIGOPS(nBestHeight))
                continue;
            //if( fDebug ){ printf("CreateNewBlock : check 1 pass \n"); }
            // Timestamp limit
            if (tx.nTime > GetAdjustedTime() || (fProofOfStake && tx.nTime > pblock->vtx[0].nTime))
                continue;
            //if( fDebug ){ printf("CreateNewBlock : check 2 pass \n"); }
            // Transaction fee
            int64_t nMinFee = tx.GetMinFee(nBlockSize, GMF_BLOCK);

            // Skip free transactions if we're past the minimum block size:
            if (fSortedByFee && (dFeePerKb < nMinTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
                continue;
            //if( fDebug ){ printf("CreateNewBlock : check 3 pass \n"); }
            // Prioritize by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee &&
                ((nBlockSize + nTxSize >= nBlockPrioritySize) || (dPriority < COIN * 144 / 250)))
            {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            // Connecting shouldn't fail due to dependency on other memory pool transactions
            // because we're already processing them in order of dependency
            map<uint256, CTxIndex> mapTestPoolTmp(mapTestPool);
            MapPrevTx mapInputs;
            bool fInvalid;
            if (!tx.FetchInputs(txdb, mapTestPoolTmp, false, true, mapInputs, fInvalid))
                continue;

            uint64_t u6ToMinerFee=0;      bool fBitBetEncashTx = isBitBetEncashTx(tx, u6ToMinerFee);  // will check details in IsStandardTx() -> acceptBitBetTx();
            std::string sHash = tx.GetHash().ToString();
            int64_t nTxFees = 0, nVO = tx.GetValueOut();
			if( !fBitBetEncashTx ){ nTxFees = tx.GetValueIn(mapInputs) - nVO; }  //if( !fBitBetEncashTx ){ nTxFees = tx.GetValueIn(mapInputs)-tx.GetValueOut(); }
			else{
				nTxFees = u6ToMinerFee * COIN;   /*double d2 = (double)nVO * BitBet_RewardMiner_Rate;
				nTxFees = (uint64_t)d2;  // 1498 * COIN * 0.001 = 1.498
				//if( ov >= BitBet_MiniTxOutValue_For_RewardMiner ){ nTxFees = (int64_t)((double)(ov * BitBet_RewardMiner_Rate)); }  // 100000 COINS,   
#ifdef WIN32
				double d3=d2 / COIN;
				if( fDebug ){ printf("CreateNewBlock : tx.GetValueOut()=[%I64u], nTxFees=[%I64u] [%lf] :: [%lf] \n", nVO, (nTxFees / COIN), d2, d3); }
#endif
				if( nTxFees < COIN ){ nTxFees = COIN; }  //if( nTxFees < MIN_TX_FEE ){ nTxFees = MIN_TX_FEE; }
				else{ uint64_t u6m = nTxFees % COIN;   if( u6m >= (COIN / 2) ){ nTxFees += COIN; } } */
			}
            if( fDebug ){ printf("CreateNewBlock : nTxFees=[%s],  nMinFee=[%s] \n", u64tostr(nTxFees).c_str(), u64tostr(nMinFee).c_str()); }
            if( !fBitBetEncashTx && (nTxFees < nMinFee) )
                continue;
            //if( fDebug ){ printf("CreateNewBlock : check 5 pass \n"); }
            nTxSigOps += tx.GetP2SHSigOpCount(mapInputs);
            if (nBlockSigOps + nTxSigOps >= max_BLOCK_SIGOPS(nBestHeight))
                continue;
            //if( fDebug ){ printf("CreateNewBlock : check 6 pass \n"); }
            if (!tx.ConnectInputs(txdb, mapInputs, mapTestPoolTmp, CDiskTxPos(1,1,1), pindexPrev, false, true, fBitBetEncashTx))
                continue;
            //if( fDebug ){ printf("CreateNewBlock : check 7 pass \n"); }
            mapTestPoolTmp[tx.GetHash()] = CTxIndex(CDiskTxPos(1,1,1), tx.vout.size());
            swap(mapTestPool, mapTestPoolTmp);

            // Added
            pblock->vtx.push_back(tx);      //if( fDebug ){ printf("CreateNewBlock : add tx [%s] to pblock, fBitBetEncashTx=[%d] \n", sHash.c_str(), fBitBetEncashTx); }
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;
#ifdef WIN32
            if( fDebug ){ printf("CreateNewBlock : fBitBetEncashTx=[%d], nFees=[%I64u] :: [%I64u], tx.Hash()=[%s] \n", fBitBetEncashTx, nFees, nTxFees / COIN, sHash.c_str()); }
#endif

            if (fDebug && GetBoolArg("-printpriority"))
            {
                printf("priority %.1f feeperkb %.1f txid %s\n",
                       dPriority, dFeePerKb, tx.GetHash().ToString().c_str());
            }

            // Add transactions that depend on this one to the priority queue
            uint256 hash = tx.GetHash();
            if (mapDependers.count(hash))
            {
                BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                {
                    if (!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty())
                        {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->dFeePerKb, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;

        if (fDebug && GetBoolArg("-printpriority"))
            printf("CreateNewBlock(): total size %"PRIu64"\n", nBlockSize);

        if (!fProofOfStake)
            pblock->vtx[0].vout[0].nValue = GetProofOfWorkReward(nFees);

        if (pFees)
            *pFees = nFees;

        // Fill in header
        pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
        pblock->nTime          = max(pindexPrev->GetPastTimeLimit()+1, pblock->GetMaxTransactionTime());
        pblock->nTime          = max(pblock->GetBlockTime(), PastDrift(pindexPrev->GetBlockTime(), nHeight));
        if( !fProofOfStake ){ pblock->UpdateTime(pindexPrev); }
        else if( bQPoS_Rules_Actived ){ pblock->nTime = GetAdjustedTime(); }
        pblock->nNonce         = 0;
    }

    return pblock.release();
}


void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
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
    pblock->vtx[0].vin[0].scriptSig = (CScript() << nHeight << CBigNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(pblock->vtx[0].vin[0].scriptSig.size() <= 100);

    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}


void FormatHashBuffers(CBlock* pblock, char* pmidstate, char* pdata, char* phash1)
{
    //
    // Pre-build hash buffers
    //
    struct
    {
        struct unnamed2
        {
            int nVersion;
            uint256 hashPrevBlock;
            uint256 hashMerkleRoot;
            unsigned int nTime;
            unsigned int nBits;
            unsigned int nNonce;
        }
        block;
        unsigned char pchPadding0[64];
        uint256 hash1;
        unsigned char pchPadding1[64];
    }
    tmp;
    memset(&tmp, 0, sizeof(tmp));

    tmp.block.nVersion       = pblock->nVersion;
    tmp.block.hashPrevBlock  = pblock->hashPrevBlock;
    tmp.block.hashMerkleRoot = pblock->hashMerkleRoot;
    tmp.block.nTime          = pblock->nTime;
    tmp.block.nBits          = pblock->nBits;
    tmp.block.nNonce         = pblock->nNonce;

    FormatHashBlocks(&tmp.block, sizeof(tmp.block));
    FormatHashBlocks(&tmp.hash1, sizeof(tmp.hash1));

    // Byte swap all the input buffer
    for (unsigned int i = 0; i < sizeof(tmp)/4; i++)
        ((unsigned int*)&tmp)[i] = ByteReverse(((unsigned int*)&tmp)[i]);

    // Precalc the first half of the first hash, which stays constant
    SHA256Transform(pmidstate, &tmp.block, pSHA256InitState);

    memcpy(pdata, &tmp.block, 128);
    memcpy(phash1, &tmp.hash1, 64);
}


bool CheckWork(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
    uint256 hashBlock = pblock->GetHash();
    uint256 hashProof = pblock->GetPoWHash();
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

    if(!pblock->IsProofOfWork())
        return error("CheckWork() : %s is not a proof-of-work block", hashBlock.GetHex().c_str());

    if (hashProof > hashTarget)
        return error("CheckWork() : proof-of-work not meeting target");

    //// debug print
    printf("CheckWork() : new proof-of-work block found  \n  proof hash: %s  \ntarget: %s\n", hashProof.GetHex().c_str(), hashTarget.GetHex().c_str());
    pblock->print();
    printf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue).c_str());

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != hashBestChain)
            return error("CheckWork() : generated block is stale");

        // Remove key from key pool
        reservekey.KeepKey();

        // Track how many getdata requests this block gets
        {
            LOCK(wallet.cs_wallet);
            wallet.mapRequestCount[hashBlock] = 0;
        }

        // Process this block the same as if we had received it from another node
        if (!ProcessBlock(NULL, pblock))
            return error("CheckWork() : ProcessBlock, block not accepted");
    }

    return true;
}

bool CheckStake(CBlock* pblock, CWallet& wallet)
{
    uint256 proofHash = 0, hashTarget = 0;
    uint256 hashBlock = pblock->GetHash();

    if(!pblock->IsProofOfStake())
        return error("CheckStake() : %s is not a proof-of-stake block", hashBlock.GetHex().c_str());

    // verify hash target and signature of coinstake tx
    if (!CheckProofOfStake(mapBlockIndex[pblock->hashPrevBlock], pblock->vtx[1], pblock->nBits, proofHash, hashTarget))
        return error("CheckStake() : proof-of-stake checking failed");

    //// debug print
	uint64_t aHei = getTxBlockHeightBy_hashBlock(pblock->hashPrevBlock);
	if( aHei > 0 ){ aHei++; }
    printf("CheckStake() : new proof-of-stake block found [%s : %s : %s]  \n  hash: %s \nproofhash: %s  \ntarget: %s\n", u64tostr(aHei).c_str(), u64tostr(nEstimateHeight).c_str(), u64tostr(nBestHeight).c_str(), hashBlock.GetHex().c_str(), proofHash.GetHex().c_str(), hashTarget.GetHex().c_str());
    pblock->print();
    printf("out %s\n", FormatMoney(pblock->vtx[1].GetValueOut()).c_str());
	if( (nEstimateHeight > 0) && (aHei <= nEstimateHeight) )
    {
        return error("CheckStake() : generated block is stale, block height [%s <= %s, %s]", u64tostr(aHei).c_str(), u64tostr(nEstimateHeight).c_str(), u64tostr(nBestHeight).c_str());
    }

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != hashBestChain)
            return error("CheckStake() : generated block is stale");

        // Track how many getdata requests this block gets
        {
            LOCK(wallet.cs_wallet);
            wallet.mapRequestCount[hashBlock] = 0;
        }

        // Process this block the same as if we had received it from another node
        if (!ProcessBlock(NULL, pblock))
            return error("CheckStake() : ProcessBlock, block not accepted");
    }

    return true;
}

void StakeMiner(CWallet *pwallet)
{
    SetThreadPriority(THREAD_PRIORITY_LOWEST);

    // Make this thread recognisable as the mining thread
    RenameThread("luckchain-miner");

    bool fTryToSync = true,  bIgnoreBlocksOfPeers = GetBoolArg("-minerignoreblkofpeers", false);
    int iQPoSMiniConnects = GetArg("-qposminiconnects", (fTestNet ? 3 : 8));

    while (true)
    {
        if (fShutdown)
            return;

        while (pwallet->IsLocked())
        {
            nLastCoinStakeSearchInterval = 0;
            MilliSleep(1000);
            if (fShutdown)
                return;
        }

        while (vNodes.empty() || IsInitialBlockDownload())
        {
            nLastCoinStakeSearchInterval = 0;
            fTryToSync = true;
            MilliSleep(1000);
            if (fShutdown)
                return;
        }

        if (fTryToSync)
        {
            fTryToSync = false;
            if (GetNumConnections() < 1 || nBestHeight < GetNumBlocksOfPeers())
            {
                MilliSleep(60000);
                continue;
            }
        }

        bool bQPoS_Rules_Actived = Is_Queue_PoS_Rules_Acitved(nBestHeight + 1);
        if( !bQPoS_Rules_Actived )
        {
            balancedMining();
            if( !bNormalMinerWeight )
            {
                for(int i=0; i<600; i++)
                {
                    MilliSleep(1000);
                    if (fShutdown){  return;  }
                }
                return;
            }
        }

        //
        // Create new block
        //
        int64_t nFees;
        auto_ptr<CBlock> pblock(CreateNewBlock(pwallet, true, &nFees));
        if( !pblock.get() ){ return; }

        bool bOk = true;
        if( bQPoS_Rules_Actived )
	    {
            if( strLocalPublicIP.length() < Mini_IP_Length ){ bOk = false; }
            else{
                int iMinNodeCount = iQPoSMiniConnects;      bool bNeedSync = false;
		        if( bSystemNodeWallet )
			    {
				    bNeedSync = nBestHeight < GetNumBlocksOfPeers();  // iMinNodeCount = iQPoSMiniConnects;
				    if( bNeedSync && bIgnoreBlocksOfPeers ){ bNeedSync = false; }
			    }
			    if( (GetNumConnections() < iMinNodeCount) || bNeedSync )
                {
                    for(int i=0; i<30; i++)
                    {
                        MilliSleep(500);
                        if (fShutdown){  return;  }
                    }
                    continue;
                }

	    	    if( !isTheRightMiningTime() ){ bOk = false; }
		        else
    		    {
	    		    if( !ImTheCurrentQueueMiner() ){ bOk = false; }
    				else{
	    			//int64_t blkHeight = strToInt64(pblock->blockData.c_str());
		        	CBlockIndex* pindexPrev = pindexBest;
			        int64_t nHeight = pindexPrev->nHeight + 1,  blkTm = pblock->GetBlockTime(),  prevBlkTm = pindexPrev->GetBlockTime();
			        int64_t blkSpace = blkTm - prevBlkTm,  tmNow = GetAdjustedTime();
			        bOk = (blkSpace >= Queue_Node_Block_Min_Interval);  // bOk = (nHeight == blkHeight) && (blkSpace >= Queue_Node_Block_Min_Interval);  // 59
    				/*if( bOk )
	    			{
		    			blkSpace = tmNow - prevBlkTm;
			    	}*/
			        }
		        }
			}
    	}
#ifdef WIN32
        std::string sHash = pblock->GetHash().ToString();
        if( GetArg("-printminerinfo", 0) ){ printf("StakeMiner : bOk=[%d], bTimeSyncedFromNtpServer=[%d], SyncBlockChain=[%d], pblock.Hash()=[%s] nFees=[%I64u] \n", bOk, bTimeSyncedFromNtpServer, bMiner_SyncBlockChain, sHash.c_str(), nFees / COIN); }
#endif

        // Trying to sign a block
        if( bOk && bTimeSyncedFromNtpServer && pblock->SignBlock(*pwallet, nFees) )  //if( (!bMiner_SyncBlockChain) && (pblock->SignBlock(*pwallet, nFees)) )
        {
            SetThreadPriority(THREAD_PRIORITY_NORMAL);
            CheckStake(pblock.get(), *pwallet);
            SetThreadPriority(THREAD_PRIORITY_LOWEST);
            if( !bQPoS_Rules_Actived ){ balancedMining(); }
        }
		MilliSleep(nMinerSleep);
        if( bQPoS_Rules_Actived ){ MilliSleep(1000); }
    }
}
