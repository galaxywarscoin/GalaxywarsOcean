// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/******************************************************************************
 * Copyright © 2014-2019 The SuperNET Developers.                             *
 *                                                                            *
 * See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
 * the top-level directory of this distribution for the individual copyright  *
 * holder information and the developer policies on copyright and licensing.  *
 *                                                                            *
 * Unless otherwise agreed in a custom licensing agreement, no part of the    *
 * SuperNET software, including this file may be copied, modified, propagated *
 * or distributed except according to the terms contained in the LICENSE file *
 *                                                                            *
 * Removal or modification of this copyright notice is prohibited.            *
 *                                                                            *
 ******************************************************************************/

#include "pubkey.h"
#include "miner.h"
#include "komodo_utils.h"
#include "komodo_globals.h"
#include "komodo_bitcoind.h"
#include "komodo_gateway.h"
#include "komodo_defs.h"
#include "cc/CCinclude.h"
#ifdef ENABLE_MINING
#include "pow/tromp/equi_miner.h"
#endif

#include "amount.h"
#include "chainparams.h"
#include "importcoin.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#ifdef ENABLE_MINING
#include "crypto/equihash.h"
#endif
#include "hash.h"
#include "key_io.h"
#include "main.h"
#include "metrics.h"
#include "net.h"
#include "pow.h"
#include "primitives/transaction.h"
#include "random.h"
#include "timedata.h"
#include "ui_interface.h"
#include "util.h"
#include "utilmoneystr.h"
#include "hex.h"

#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include "zcash/Address.hpp"
#include "transaction_builder.h"

#include "sodium.h"

#include "notaries_staked.h"
#include "komodo_notary.h"
#include "komodo_bitcoind.h"
#include "komodo_extern_globals.h"

#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>
#ifdef ENABLE_MINING
#include <functional>
#endif
#include <mutex>

using namespace std;

//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest priority or fee rate, so we might consider
// transactions that depend on transactions that aren't yet in the block.
// The COrphan class keeps track of these 'temporary orphans' while
// CreateBlock is figuring out which transactions to include.
//
class COrphan
{
public:
    const CTransaction* ptx;
    set<uint256> setDependsOn;
    CFeeRate feeRate;
    double dPriority;

    COrphan(const CTransaction* ptxIn) : ptx(ptxIn), feeRate(0), dPriority(0)
    {
    }
};

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;

// We want to sort transactions by priority and fee rate, so:
typedef boost::tuple<double, CFeeRate, const CTransaction*> TxPriority;
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

void UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    if ( ASSETCHAINS_ADAPTIVEPOW <= 0 )
        pblock->nTime = std::max(pindexPrev->GetMedianTimePast()+1, GetTime());
    else pblock->nTime = std::max((int64_t)(pindexPrev->nTime+1), GetTime());

    // Updating time can change work required on testnet:
    if (ASSETCHAINS_ADAPTIVEPOW > 0 || consensusParams.nPowAllowMinDifficultyBlocksAfterHeight != boost::none)
    {
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);
    }
}

extern CCriticalSection cs_metrics;

uint32_t Mining_start,Mining_height;
int32_t My_notaryid = -1;

int32_t komodo_waituntilelegible(uint32_t blocktime, int32_t stakeHeight, uint32_t delay)
{
    int64_t adjustedtime = (int64_t)GetTime();
    while ( (int64_t)blocktime-ASSETCHAINS_STAKED_BLOCK_FUTURE_MAX > adjustedtime )
    {
        boost::this_thread::interruption_point(); // allow to interrupt
        int64_t secToElegible = (int64_t)blocktime-ASSETCHAINS_STAKED_BLOCK_FUTURE_MAX-adjustedtime;
        if ( delay <= ASSETCHAINS_STAKED_BLOCK_FUTURE_HALF && secToElegible <= ASSETCHAINS_STAKED_BLOCK_FUTURE_HALF )
            break;
        if ( (rand() % 100) < 2-(secToElegible>ASSETCHAINS_STAKED_BLOCK_FUTURE_MAX) ) 
            LogPrintf( "[%s:%i] %llds until elegible...\n", chainName.symbol().c_str(), stakeHeight, (long long)secToElegible);
        if ( chainActive.Tip()->nHeight >= stakeHeight )
        {
            LogPrintf( "[%s:%i] Chain advanced, reset staking loop.\n", chainName.symbol().c_str(), stakeHeight);
            return(0);
        }
        if( !GetBoolArg("-gen",false) ) 
            return(0);
        //sleep(1);
        boost::this_thread::sleep_for(boost::chrono::seconds(1)); // allow to interrupt
        adjustedtime = (int64_t)GetTime();
    } 
    return(1);
}

/*****
 * @breif Generate a new block based on mempool txs, without valid proof-of-work 
 * @param _pk the public key
 * @param _scriptPubKeyIn the script for the public key
 * @param gpucount assists in calculating the block's nTime
 * @param isStake
 * @returns the block template
 */
CBlockTemplate* CreateNewBlock(const CPubKey _pk,const CScript& _scriptPubKeyIn, int32_t gpucount, bool isStake)
{
    CScript scriptPubKeyIn(_scriptPubKeyIn);

    CPubKey pk;
    if ( _pk.size() != 33 )
    {
        pk = CPubKey();
        std::vector<std::vector<unsigned char>> vAddrs;
        txnouttype txT;
        if ( scriptPubKeyIn.size() > 0 && Solver(scriptPubKeyIn, txT, vAddrs))
        {
            if (txT == TX_PUBKEY)
                pk = CPubKey(vAddrs[0]);
        }
    } else pk = _pk;

    
    uint32_t blocktime; 
    const CChainParams& chainparams = Params();
    bool fNotarisationBlock = false; 
    std::vector<int8_t> NotarisationNotaries;

    // Create new block
    if ( gpucount < 0 )
        gpucount = KOMODO_MAXGPUCOUNT;
    std::unique_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    if(!pblocktemplate.get())
    {
        LogPrintf("pblocktemplate.get() failure\n");
        return NULL;
    }
    CBlock *pblock = &pblocktemplate->block; // pointer for convenience
     // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (Params().MineBlocksOnDemand())
        pblock->nVersion = GetArg("-blockversion", pblock->nVersion);

    // Add dummy coinbase tx as first transaction
    pblock->vtx.push_back(CTransaction());
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); // updated at end

    CBlockIndex *tipindex = nullptr;
    {
        LOCK(cs_main);
        tipindex = chainActive.Tip();
    }
    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", MAX_BLOCK_SIZE(tipindex->nHeight+1));
    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE(tipindex->nHeight+1)-1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", DEFAULT_BLOCK_MIN_SIZE);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Collect memory pool transactions into the block
    CAmount nFees = 0;

    boost::this_thread::interruption_point(); // exit thread before entering locks. 
    
    CBlockIndex* pindexPrev = 0;
    {
        // this should stop create block ever exiting until it has returned something. 
        boost::this_thread::disable_interruption();
        ENTER_CRITICAL_SECTION(cs_main);
        ENTER_CRITICAL_SECTION(mempool.cs);
        pindexPrev = chainActive.Tip();
        const int nHeight = pindexPrev->nHeight + 1;
        const Consensus::Params &consensusParams = chainparams.GetConsensus();
        uint32_t consensusBranchId = CurrentEpochBranchId(nHeight, consensusParams);
        bool sapling = NetworkUpgradeActive(nHeight, consensusParams, Consensus::UPGRADE_SAPLING);

        const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();
        uint32_t proposedTime = GetTime();

        if (proposedTime == nMedianTimePast)
        {
            // too fast or stuck, this addresses the too fast issue, while moving
            // forward as quickly as possible
            for (int i = 0; i < 100; i++)
            {
                proposedTime = GetTime();
                if (proposedTime == nMedianTimePast)
                    MilliSleep(10); // allow to interrupt
            }
        }
        pblock->nTime = GetTime();
        // Now we have the block time + height, we can get the active notaries.
        int8_t numSN = 0; uint8_t notarypubkeys[64][33] = {0};
        if ( ASSETCHAINS_NOTARY_PAY[0] != 0 )
        {
            // Only use speical miner for notary pay chains.
            numSN = komodo_notaries(notarypubkeys, nHeight, pblock->nTime);
        }

        CCoinsViewCache view(pcoinsTip);

        SaplingMerkleTree sapling_tree;
        assert(view.GetSaplingAnchorAt(view.GetBestAnchor(SAPLING), sapling_tree));

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;
        bool fPrintPriority = GetBoolArg("-printpriority", false);

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size() + 1);

        // now add transactions from the mem pool
        int32_t Notarisations = 0; uint64_t txvalue;
        for (CTxMemPool::indexed_transaction_set::iterator mi = mempool.mapTx.begin();
             mi != mempool.mapTx.end(); ++mi)
        {
            //break; // dont add any tx to block.. debug for KMD fix. Disabled. 
            const CTransaction& tx = mi->GetTx();

            int64_t nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
            ? nMedianTimePast
            : pblock->GetBlockTime();

            if (tx.IsCoinBase() || !IsFinalTx(tx, nHeight, nLockTimeCutoff) || IsExpiredTx(tx, nHeight))
            {
                //LogPrintf("coinbase.%d finaltx.%d expired.%d\n",tx.IsCoinBase(),IsFinalTx(tx, nHeight, nLockTimeCutoff),IsExpiredTx(tx, nHeight));
                continue;
            }
            txvalue = tx.GetValueOut();
            if ( KOMODO_VALUETOOBIG(txvalue) != 0 )
                continue;

            /* HF22 - check interest validation against pindexPrev->GetMedianTimePast() + 777 */
            uint32_t cmptime = (uint32_t)pblock->nTime;

            if (chainName.isKMD() &&
                consensusParams.nHF22Height != boost::none && nHeight > consensusParams.nHF22Height.get()
            ) {
                uint32_t cmptime_old = cmptime;
                cmptime = nMedianTimePast + 777;
                LogPrint("hfnet","%s[%d]: cmptime.%lu -> %lu\n", __func__, __LINE__, cmptime_old, cmptime);
                LogPrint("hfnet","%s[%d]: ht.%ld\n", __func__, __LINE__, nHeight);
            }

            if (chainName.isKMD() && !komodo_validate_interest(tx, nHeight, cmptime))
            {
                LogPrintf("%s: komodo_validate_interest failure txid.%s nHeight.%d nTime.%u vs locktime.%u (cmptime.%lu)\n", __func__, tx.GetHash().ToString(), nHeight, (uint32_t)pblock->nTime, (uint32_t)tx.nLockTime, cmptime);
                continue;
            }

            COrphan* porphan = NULL;
            double dPriority = 0;
            CAmount nTotalIn = 0;
            bool fMissingInputs = false;
            bool fNotarisation = false;
            std::vector<int8_t> TMP_NotarisationNotaries;
            if (tx.IsCoinImport())
            {
                CAmount nValueIn = GetCoinImportValue(tx); // burn amount
                nTotalIn += nValueIn;
                dPriority += (double)nValueIn * 1000;  // flat multiplier... max = 1e16.
            } else {
                TMP_NotarisationNotaries.clear();
                bool fToCryptoAddress = false;
                if ( numSN != 0 && notarypubkeys[0][0] != 0 && komodo_is_notarytx(tx) == 1 )
                    fToCryptoAddress = true;

                BOOST_FOREACH(const CTxIn& txin, tx.vin)
                {
                    // Read prev transaction
                    if (!view.HaveCoins(txin.prevout.hash))
                    {
                        // This should never happen; all transactions in the memory
                        // pool should connect to either transactions in the chain
                        // or other transactions in the memory pool.
                        if (!mempool.mapTx.count(txin.prevout.hash))
                        {
                            LogPrintf("ERROR: mempool transaction missing input\n");
                            // if (fDebug) assert("mempool transaction missing input" == 0);
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
                        nTotalIn += mempool.mapTx.find(txin.prevout.hash)->GetTx().vout[txin.prevout.n].nValue;
                        continue;
                    }
                    const CCoins* coins = view.AccessCoins(txin.prevout.hash);
                    assert(coins);

                    CAmount nValueIn = coins->vout[txin.prevout.n].nValue;
                    nTotalIn += nValueIn;

                    int nConf = nHeight - coins->nHeight;
                    
                    uint8_t *script; int32_t scriptlen; uint256 hash; CTransaction tx1;
                    // loop over notaries array and extract index of signers.
                    if ( fToCryptoAddress && myGetTransaction(txin.prevout.hash,tx1,hash) )
                    {
                        for (int8_t i = 0; i < numSN; i++) 
                        {
                            script = (uint8_t *)&tx1.vout[txin.prevout.n].scriptPubKey[0];
                            scriptlen = (int32_t)tx1.vout[txin.prevout.n].scriptPubKey.size();
                            if ( scriptlen == 35 && script[0] == 33 && script[34] == OP_CHECKSIG && memcmp(script+1,notarypubkeys[i],33) == 0 )
                            {
                                // We can add the index of each notary to vector, and clear it if this notarisation is not valid later on.
                                TMP_NotarisationNotaries.push_back(i);                          
                            }
                        }
                    }
                    dPriority += (double)nValueIn * nConf;
                }
                if ( numSN != 0 && notarypubkeys[0][0] != 0 && TMP_NotarisationNotaries.size() >= numSN / 5 )
                {
                    // check a notary didnt sign twice (this would be an invalid notarisation later on and cause problems)
                    std::set<int> checkdupes( TMP_NotarisationNotaries.begin(), TMP_NotarisationNotaries.end() );
                    if ( checkdupes.size() != TMP_NotarisationNotaries.size() ) 
                    {
                        LogPrintf( "possible notarisation is signed multiple times by same notary, passed as normal transaction.\n");
                    } else fNotarisation = true;
                }
                nTotalIn += tx.GetShieldedValueIn();
            }

            if (fMissingInputs) continue;

            // Priority is sum(valuein * age) / modified_txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority = tx.ComputePriority(dPriority, nTxSize);

            uint256 hash = tx.GetHash();
            mempool.ApplyDeltas(hash, dPriority, nTotalIn);

            CFeeRate feeRate(nTotalIn-tx.GetValueOut(), nTxSize);

            if ( fNotarisation ) 
            {
                // Special miner for notary pay chains. Can only enter this if numSN/notarypubkeys is set higher up.
                if ( tx.vout.size() == 2 && tx.vout[1].nValue == 0 )
                {
                    // Get the OP_RETURN for the notarisation
                    uint8_t *script = (uint8_t *)&tx.vout[1].scriptPubKey[0];
                    int32_t scriptlen = (int32_t)tx.vout[1].scriptPubKey.size();
                    if ( script[0] == OP_RETURN )
                    {
                        Notarisations++;
                        if ( Notarisations > 1 ) 
                        {
                            LogPrintf( "skipping notarization.%d\n",Notarisations);
                            // Any attempted notarization needs to be in its own block!
                            continue;
                        }
                        int32_t notarizedheight = komodo_getnotarizedheight(pblock->nTime, nHeight, script, scriptlen);
                        if ( notarizedheight != 0 )
                        {
                            // this is the first one we see, add it to the block as TX1 
                            NotarisationNotaries = TMP_NotarisationNotaries;
                            dPriority = 1e16;
                            fNotarisationBlock = true;
                            //LogPrintf( "Notarisation %s set to maximum priority\n",hash.ToString().c_str());
                        }
                    }
                }
            }
            else if ( dPriority == 1e16 )
            {
                dPriority -= 10;
                // make sure notarisation is tx[1] in block. 
            }
            if (porphan)
            {
                porphan->dPriority = dPriority;
                porphan->feeRate = feeRate;
            }
            else
                vecPriority.push_back(TxPriority(dPriority, feeRate, &(mi->GetTx())));
        }

        // Collect transactions into block
        uint64_t nBlockSize = 1000;
        uint64_t nBlockTx = 0;
        int64_t interest;
        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty())
        {
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            CFeeRate feeRate = vecPriority.front().get<1>();
            const CTransaction& tx = *(vecPriority.front().get<2>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);

            // Opret spam limits
            if (mapArgs.count("-opretmintxfee"))
            {
                CAmount n = 0;
                CFeeRate opretMinFeeRate;
                if (ParseMoney(mapArgs["-opretmintxfee"], n) && n > 0)
                    opretMinFeeRate = CFeeRate(n);
                else
                    opretMinFeeRate = CFeeRate(400000); // default opretMinFeeRate (1 KMD per 250 Kb = 0.004 per 1 Kb = 400000 sat per 1 Kb)

                bool fSpamTx = false;
                unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
                unsigned int nTxOpretSize = 0;

                // calc total oprets size
                BOOST_FOREACH(const CTxOut& txout, tx.vout) {
                    if (txout.scriptPubKey.IsOpReturn()) {
                        CScript::const_iterator it = txout.scriptPubKey.begin() + 1;
                        opcodetype op;
                        std::vector<uint8_t> opretData;
                        if (txout.scriptPubKey.GetOp(it, op, opretData)) {
                            //std::cerr << HexStr(opretData.begin(), opretData.end()) << std::endl;
                            nTxOpretSize += opretData.size();
                        }
                    }
                }

                if ((nTxOpretSize > 256) && (feeRate < opretMinFeeRate)) fSpamTx = true;
                // std::cerr << tx.GetHash().ToString() << " nTxSize." << nTxSize << " nTxOpretSize." << nTxOpretSize << " feeRate." << feeRate.ToString() << " opretMinFeeRate." << opretMinFeeRate.ToString() << " fSpamTx." << fSpamTx << std::endl;
                if (fSpamTx) continue;
                // std::cerr << tx.GetHash().ToString() << " vecPriority.size() = " << vecPriority.size() << std::endl;
            }

            if (nBlockSize + nTxSize >= nBlockMaxSize-512) // room for extra autotx
            {
                //LogPrintf("nBlockSize %d + %d nTxSize >= %d nBlockMaxSize\n",(int32_t)nBlockSize,(int32_t)nTxSize,(int32_t)nBlockMaxSize);
                continue;
            }

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = GetLegacySigOpCount(tx);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS-1)
            {
                //LogPrintf("A nBlockSigOps %d + %d nTxSigOps >= %d MAX_BLOCK_SIGOPS-1\n",(int32_t)nBlockSigOps,(int32_t)nTxSigOps,(int32_t)MAX_BLOCK_SIGOPS);
                continue;
            }
            // Skip free transactions if we're past the minimum block size:
            const uint256& hash = tx.GetHash();
            double dPriorityDelta = 0;
            CAmount nFeeDelta = 0;
            mempool.ApplyDeltas(hash, dPriorityDelta, nFeeDelta);
            if (fSortedByFee && (dPriorityDelta <= 0) && (nFeeDelta <= 0) && (feeRate < ::minRelayTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
            {
                //LogPrintf("fee rate skip\n");
                continue;
            }
            // Prioritise by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee &&
                ((nBlockSize + nTxSize >= nBlockPrioritySize) || !AllowFree(dPriority)))
            {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            if (!view.HaveInputs(tx))
            {
                //LogPrintf("dont have inputs\n");
                continue;
            }
            CAmount nTxFees = view.GetValueIn(chainActive.Tip()->nHeight, interest, tx) - tx.GetValueOut();

            nTxSigOps += GetP2SHSigOpCount(tx, view);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS-1)
            {
                //LogPrintf("B nBlockSigOps %d + %d nTxSigOps >= %d MAX_BLOCK_SIGOPS-1\n",(int32_t)nBlockSigOps,(int32_t)nTxSigOps,(int32_t)MAX_BLOCK_SIGOPS);
                continue;
            }
            // Note that flags: we don't want to set mempool/IsStandard()
            // policy here, but we still have to ensure that the block we
            // create only contains transactions that are valid in new blocks.
            CValidationState state;
            PrecomputedTransactionData txdata(tx);
            if (!ContextualCheckInputs(tx, state, view, true, MANDATORY_SCRIPT_VERIFY_FLAGS, true, txdata, Params().GetConsensus(), consensusBranchId))
            {
                //LogPrintf("context failure\n");
                continue;
            }
            UpdateCoins(tx, view, nHeight);

            BOOST_FOREACH(const OutputDescription &outDescription, tx.vShieldedOutput) {
                sapling_tree.append(outDescription.cm);
            }

            // Added
            pblock->vtx.push_back(tx);
            pblocktemplate->vTxFees.push_back(nTxFees);
            pblocktemplate->vTxSigOps.push_back(nTxSigOps);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fPrintPriority)
            {
                LogPrintf("priority %.1f fee %s txid %s\n",dPriority, feeRate.ToString(), tx.GetHash().ToString());
            }

            // Add transactions that depend on this one to the priority queue
            if (mapDependers.count(hash))
            {
                BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                {
                    if (!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty())
                        {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->feeRate, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;
        if ( ASSETCHAINS_ADAPTIVEPOW <= 0 )
            blocktime = 1 + std::max(pindexPrev->GetMedianTimePast()+1, GetTime());
        else blocktime = 1 + std::max((int64_t)(pindexPrev->nTime+1), GetTime());

        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, Params().GetConsensus());

        int32_t stakeHeight = chainActive.Height() + 1;

        //LogPrintf("CreateNewBlock(): total size %u blocktime.%u nBits.%08x stake.%i\n", nBlockSize,blocktime,pblock->nBits,isStake);
        if ( !chainName.isKMD() && isStake )
        {
            LEAVE_CRITICAL_SECTION(cs_main);
            LEAVE_CRITICAL_SECTION(mempool.cs);
            uint64_t txfees,utxovalue; uint32_t txtime; uint256 utxotxid; int32_t i,siglen,numsigs,utxovout; uint8_t utxosig[512],*ptr;
            CMutableTransaction txStaked = CreateNewContextualCMutableTransaction(Params().GetConsensus(), stakeHeight);

            blocktime = GetTime();
            uint256 merkleroot = komodo_calcmerkleroot(pblock, pindexPrev->GetBlockHash(), nHeight, true, scriptPubKeyIn);
            //LogPrintf( "MINER: merkleroot.%s\n", merkleroot.GetHex().c_str());
            /*
                Instead of a split RPC and writing heaps of code, I added this.
                It works with the consensus, because the stakeTx valuein-valueout+blockReward is enforced for the coinbase of staking blocks.
                For example:
                utxovalue = 30;
                30% of the value of the staking utxo is added to the coinbase the same as fees,returning the remaining 70% to the address that staked.
                utxovalue can be adjusted from any number 0 to 100 via the setstakingsplit RPC.
                Can also be set with -splitperc= command line arg, or conf file.
                0 means that it functions as it did previously (default).
                100 means it automates the pos64staker in the daemon, combining the stake tx and the coinbase to an address. Either to -pubkey or a new address from the keystore.
                Mining with a % here and without pubkey will over time create varied sized utxos, and evenly distribute them over many addresses and segids.
            */
            {
                    LOCK(cs_main);
                    utxovalue = ASSETCHAINS_STAKED_SPLIT_PERCENTAGE;
            }

            siglen = komodo_staked(txStaked, pblock->nBits, &blocktime, &txtime, &utxotxid, &utxovout, &utxovalue, utxosig, merkleroot);
            if ( komodo_newStakerActive(nHeight, blocktime) != 0 )
                nFees += utxovalue;
            //LogPrintf( "added to coinbase.%llu staking tx valueout.%llu\n", (long long unsigned)utxovalue, (long long unsigned)txStaked.vout[0].nValue);
            uint32_t delay = ASSETCHAINS_ALGO != ASSETCHAINS_EQUIHASH ? ASSETCHAINS_STAKED_BLOCK_FUTURE_MAX : ASSETCHAINS_STAKED_BLOCK_FUTURE_HALF;
            if ( komodo_waituntilelegible(blocktime, stakeHeight, delay) == 0 )
                return(0);

            if ( siglen > 0 )
            {
                CAmount txfees;

                txfees = 0;

                pblock->vtx.push_back(txStaked);
                pblocktemplate->vTxFees.push_back(txfees);
                pblocktemplate->vTxSigOps.push_back(GetLegacySigOpCount(txStaked));
                nFees += txfees;
                pblock->nTime = blocktime;
            } else return(0);
        }
        
        // Create coinbase tx
        CMutableTransaction txNew = CreateNewContextualCMutableTransaction(consensusParams, nHeight);
        txNew.vin.resize(1);
        txNew.vin[0].prevout.SetNull();
        txNew.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(1)) + COINBASE_FLAGS;
        txNew.vout.resize(1);
        txNew.vout[0].scriptPubKey = scriptPubKeyIn;
        txNew.vout[0].nValue = GetBlockSubsidy(nHeight,consensusParams);
        txNew.nExpiryHeight = 0;

        if (chainName.isKMD())
        {
            bool fAdd5kSat = IS_KOMODO_NOTARY && My_notaryid >= 0;
            if (nHeight < nKIP0003Activation)
            {
                txNew.vout[0].nValue += nFees + (fAdd5kSat ? 5000 : 0);
            }
            else
            {
                // KIP0003 - fee burning
                CTxOut burnOpRet(nFees + (fAdd5kSat ? 5000 : 0), CScript() << OP_RETURN);
                txNew.vout.push_back(burnOpRet);
            }
        }
        else
        {
            txNew.vout[0].nValue += nFees;
        }

        if ( ASSETCHAINS_ADAPTIVEPOW <= 0 )
            txNew.nLockTime = std::max(pindexPrev->GetMedianTimePast()+1, GetTime());
        else txNew.nLockTime = std::max((int64_t)(pindexPrev->nTime+1), GetTime());        

        pblock->vtx[0] = txNew;

        uint64_t commission;
        if ( nHeight > 1 && !chainName.isKMD() && (ASSETCHAINS_OVERRIDE_PUBKEY33[0] != 0 || ASSETCHAINS_SCRIPTPUB.size() > 1) && (ASSETCHAINS_COMMISSION != 0 || ASSETCHAINS_FOUNDERS_REWARD != 0)  && (commission= komodo_commission((CBlock*)&pblocktemplate->block,(int32_t)nHeight)) != 0 )
        {
            uint8_t *ptr;
            txNew.vout.resize(2);
            txNew.vout[1].nValue = commission;
            if ( ASSETCHAINS_SCRIPTPUB.size() > 1 )
            {
                static bool didinit = false;
                if ( !didinit && nHeight > KOMODO_EARLYTXID_HEIGHT && KOMODO_EARLYTXID != zeroid && komodo_appendACscriptpub() )
                {
                    LogPrintf( "appended ccopreturn to ASSETCHAINS_SCRIPTPUB.%s\n", ASSETCHAINS_SCRIPTPUB.c_str());
                    didinit = true;
                }
                int32_t len = strlen(ASSETCHAINS_SCRIPTPUB.c_str());
                len >>= 1;
                txNew.vout[1].scriptPubKey.resize(len);
                ptr = (uint8_t *)&txNew.vout[1].scriptPubKey[0];
                decode_hex(ptr,len,ASSETCHAINS_SCRIPTPUB.c_str());
            }
            else
            {
                txNew.vout[1].scriptPubKey.resize(35);
                ptr = (uint8_t *)&txNew.vout[1].scriptPubKey[0];
                ptr[0] = 33;
                for (int32_t i=0; i<33; i++)
                {
                    ptr[i+1] = ASSETCHAINS_OVERRIDE_PUBKEY33[i];
                }
                ptr[34] = OP_CHECKSIG;
            }
        }
        else if ( (uint64_t)(txNew.vout[0].nValue) >= ASSETCHAINS_TIMELOCKGTE)
        {
            int32_t opretlen, p2shlen, scriptlen;
            CScriptExt opretScript = CScriptExt();
            
            txNew.vout.resize(2);
            
            // prepend time lock to original script unless original script is P2SH, in which case, we will leave the coins
            // protected only by the time lock rather than 100% inaccessible
            opretScript.AddCheckLockTimeVerify(komodo_block_unlocktime(nHeight));
            if (scriptPubKeyIn.IsPayToScriptHash() || scriptPubKeyIn.IsPayToCryptoCondition())
            {
                LogPrintf("CreateNewBlock: attempt to add timelock to pay2sh or pay2cc\n");
                if ( chainName.isKMD() ||  (!chainName.isKMD() && !isStake) )
                {
                    LEAVE_CRITICAL_SECTION(cs_main);
                    LEAVE_CRITICAL_SECTION(mempool.cs);
                }
                return 0;
            }
            
            opretScript += scriptPubKeyIn;
            
            txNew.vout[0].scriptPubKey = CScriptExt().PayToScriptHash(CScriptID(opretScript));
            txNew.vout[1].scriptPubKey = CScriptExt().OpReturnScript(opretScript, OPRETTYPE_TIMELOCK);
            txNew.vout[1].nValue = 0;
            // timelocks and commissions are currently incompatible due to validation complexity of the combination
        } 
        else if ( fNotarisationBlock && ASSETCHAINS_NOTARY_PAY[0] != 0 && pblock->vtx[1].vout.size() == 2 && pblock->vtx[1].vout[1].nValue == 0 )
        {
            // Get the OP_RETURN for the notarisation
            uint8_t *script = (uint8_t *)&pblock->vtx[1].vout[1].scriptPubKey[0];
            int32_t scriptlen = (int32_t)pblock->vtx[1].vout[1].scriptPubKey.size();
            if ( script[0] == OP_RETURN )
            {
                uint64_t totalsats = komodo_notarypay(txNew, NotarisationNotaries, pblock->nTime, nHeight, script, scriptlen);
                if ( totalsats == 0 )
                {
                    LogPrintf( "Could not create notary payment, trying again.\n");
                    if ( chainName.isKMD() ||  (!chainName.isKMD() && !isStake) )
                    {
                        LEAVE_CRITICAL_SECTION(cs_main);
                        LEAVE_CRITICAL_SECTION(mempool.cs);
                    }
                    return(0);
                }
                //LogPrintf( "Created notary payment coinbase totalsat.%lu\n",totalsats);    
            } else LogPrintf( "vout 2 of notarisation is not OP_RETURN scriptlen.%i\n", scriptlen);
        }
        pblock->vtx[0] = txNew;
        pblocktemplate->vTxFees[0] = -nFees;

        //if (!isStake || true)
        // Randomise nonce
        arith_uint256 nonce = UintToArith256(GetRandHash());

        // Clear the top 16 and bottom 16 or 24 bits (for local use as thread flags and counters)
        nonce <<= ASSETCHAINS_NONCESHIFT[ASSETCHAINS_ALGO];
        nonce >>= 16;
        pblock->nNonce = ArithToUint256(nonce);

        // Fill in header
        pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
        pblock->hashFinalSaplingRoot   = sapling_tree.root();

        // all Verus PoS chains need this data in the block at all times
        if ( chainName.isKMD() || ASSETCHAINS_STAKED == 0 || KOMODO_MININGTHREADS > 0 )
        {
            UpdateTime(pblock, Params().GetConsensus(), pindexPrev);
            pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, Params().GetConsensus());
        }
        pblock->nSolution.clear();
        pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(pblock->vtx[0]);
        if ( chainName.isKMD() && IS_KOMODO_NOTARY && My_notaryid >= 0 )
        {
            uint32_t r; CScript opret;
            CMutableTransaction txNotary = CreateNewContextualCMutableTransaction(Params().GetConsensus(), chainActive.Height() + 1);
            if ( pblock->nTime < pindexPrev->nTime+60 )
                pblock->nTime = pindexPrev->nTime + 60;
            if ( gpucount < 33 )
            {
                uint8_t tmpbuffer[40]; uint32_t r; int32_t n=0; uint256 randvals;
                memcpy(&tmpbuffer[n],&My_notaryid,sizeof(My_notaryid)), n += sizeof(My_notaryid);
                memcpy(&tmpbuffer[n],&Mining_height,sizeof(Mining_height)), n += sizeof(Mining_height);
                memcpy(&tmpbuffer[n],&pblock->hashPrevBlock,sizeof(pblock->hashPrevBlock)), n += sizeof(pblock->hashPrevBlock);
                vcalc_sha256(0,(uint8_t *)&randvals,tmpbuffer,n);
                memcpy(&r,&randvals,sizeof(r));
                pblock->nTime += (r % (33 - gpucount)*(33 - gpucount));
            }
            pblock->vtx[0] = txNew;

            if ( Mining_height > nDecemberHardforkHeight ) //December 2019 hardfork
                opret = komodo_makeopret(pblock, true);
            else
                opret.clear();

            if (komodo_notaryvin(txNotary, NOTARY_PUBKEY33, opret, pblock->nTime) > 0)
            {
                CAmount txfees = 5000;
                pblock->vtx.push_back(txNotary);
                pblocktemplate->vTxFees.push_back(txfees);
                pblocktemplate->vTxSigOps.push_back(GetLegacySigOpCount(txNotary));
                nFees += txfees;
                pblocktemplate->vTxFees[0] = -nFees;
                LogPrintf("added notaryvin includes proof.%d\n", opret.size() > 0);
            }
            else
            {
                LogPrintf("error adding notaryvin, need to create 0.0001 utxos\n");
                if ( chainName.isKMD() ||  (!chainName.isKMD() && !isStake) )
                {
                    LEAVE_CRITICAL_SECTION(cs_main);
                    LEAVE_CRITICAL_SECTION(mempool.cs);
                }
                return(0);
            }
        }
        else if ( ASSETCHAINS_CC == 0 && pindexPrev != 0 && ASSETCHAINS_STAKED == 0 && (!chainName.isKMD() || !IS_KOMODO_NOTARY || My_notaryid < 0) )
        {
            CValidationState state;
            if ( !TestBlockValidity(state, *pblock, pindexPrev, false, false)) // invokes CC checks
            {
                if ( chainName.isKMD() || (!chainName.isKMD() && !isStake) )
                {
                    LEAVE_CRITICAL_SECTION(cs_main);
                    LEAVE_CRITICAL_SECTION(mempool.cs);
                }
                return(0);
            }
        }
    }
    if ( chainName.isKMD() || (!chainName.isKMD() && !isStake) )
    {
        LEAVE_CRITICAL_SECTION(cs_main);
        LEAVE_CRITICAL_SECTION(mempool.cs);
    }
    return pblocktemplate.release();
}

//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//

#ifdef ENABLE_MINING

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
    CMutableTransaction txCoinbase(pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = txCoinbase;
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}

#ifdef ENABLE_WALLET
//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//

/*****
 * Create a new block
 * @param reserveKey
 * @param nHeight
 * @param gpucount
 * @param isStake
 * @returns the block template
 */
CBlockTemplate* CreateNewBlockWithKey(CReserveKey& reservekey, int32_t nHeight, int32_t gpucount, bool isStake)
{
    CPubKey pubkey; CScript scriptPubKey; uint8_t *script,*ptr; int32_t i,len;
    if ( nHeight == 1 && ASSETCHAINS_COMMISSION != 0 && ASSETCHAINS_SCRIPTPUB[ASSETCHAINS_SCRIPTPUB.back()] != 49 && ASSETCHAINS_SCRIPTPUB[ASSETCHAINS_SCRIPTPUB.back()-1] != 51 )
    {
        if ( ASSETCHAINS_OVERRIDE_PUBKEY33[0] != 0 )
        {
            pubkey = ParseHex(ASSETCHAINS_OVERRIDE_PUBKEY);
            scriptPubKey = CScript() << ParseHex(HexStr(pubkey)) << OP_CHECKSIG;
        }
        else 
        {
            len = strlen(ASSETCHAINS_SCRIPTPUB.c_str());
            len >>= 1;
            scriptPubKey.resize(len);
            ptr = (uint8_t *)&scriptPubKey[0];
            decode_hex(ptr,len,ASSETCHAINS_SCRIPTPUB.c_str());
        }
    }
    else if ( USE_EXTERNAL_PUBKEY != 0 )
    {
        //LogPrintf("use notary pubkey\n");
        pubkey = ParseHex(NOTARY_PUBKEY);
        scriptPubKey = CScript() << ParseHex(HexStr(pubkey)) << OP_CHECKSIG;
    }
    else
    {
        if (!GetBoolArg("-disablewallet", false)) {
            // wallet enabled
            if (!reservekey.GetReservedKey(pubkey))
                return NULL;
            scriptPubKey.clear();
            scriptPubKey = CScript() << ToByteVector(pubkey) << OP_CHECKSIG;
        } else {
            // wallet disabled
            CTxDestination dest = DecodeDestination(GetArg("-mineraddress", ""));
            if (IsValidDestination(dest)) {
                // CKeyID keyID = boost::get<CKeyID>(dest);
                // scriptPubKey = CScript() << OP_DUP << OP_HASH160 << ToByteVector(keyID) << OP_EQUALVERIFY << OP_CHECKSIG;
                scriptPubKey = GetScriptForDestination(dest);
            } else
                return NULL;
        }
    }
    return CreateNewBlock(pubkey, scriptPubKey, gpucount, isStake);
}

void komodo_sendmessage(int32_t minpeers,int32_t maxpeers,const char *message,std::vector<uint8_t> payload)
{
    int32_t numsent = 0;
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if ( pnode->hSocket == INVALID_SOCKET )
            continue;
        if ( numsent < minpeers || (rand() % 10) == 0 )
        {
            //LogPrintf("pushmessage\n");
            pnode->PushMessage(message,payload);
            if ( numsent++ > maxpeers )
                break;
        }
    }
}

void komodo_broadcast(CBlock *pblock,int32_t limit)
{
    if (IsInitialBlockDownload())
        return;
    int32_t n = 1;
    //LogPrintf("broadcast new block t.%u\n",(uint32_t)time(NULL));
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
        {
            if ( pnode->hSocket == INVALID_SOCKET )
                continue;
            if ( (rand() % n) == 0 )
            {
                pnode->PushMessage("block", *pblock);
                if ( n++ > limit )
                    break;
            }
        }
    }
    //LogPrintf("finished broadcast new block t.%u\n",(uint32_t)time(NULL));
}

static bool ProcessBlockFound(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
#else
static bool ProcessBlockFound(CBlock* pblock)
#endif // ENABLE_WALLET
{
    LogPrintf("%s\n", pblock->ToString());
    LogPrintf("generated %s height.%d\n", FormatMoney(pblock->vtx[0].vout[0].nValue),chainActive.Tip()->nHeight+1);

    // Found a solution
    {
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())
        {
            uint256 hash; int32_t i;
            hash = pblock->hashPrevBlock;
            for (i=31; i>=0; i--)
                LogPrintf("%02x",((uint8_t *)&hash)[i]);
            LogPrintf(" <- prev (stale)\n");
            hash = chainActive.Tip()->GetBlockHash();
            for (i=31; i>=0; i--)
                LogPrintf("%02x",((uint8_t *)&hash)[i]);
            LogPrintf(" <- chainTip (stale)\n");

            return error("KomodoMiner: generated block is stale");
        }
    }

#ifdef ENABLE_WALLET
    // Remove key from key pool
    if ( !IS_KOMODO_NOTARY )
    {
        if (GetArg("-mineraddress", "").empty()) {
            // Remove key from key pool
            reservekey.KeepKey();
        }
    }
#endif
    //LogPrintf("process new block\n");

    // Process this block the same as if we had received it from another node
    CValidationState state;
    if (!ProcessNewBlock(1,chainActive.Tip()->nHeight+1,state, NULL, pblock, true, NULL))
        return error("KomodoMiner: ProcessNewBlock, block not accepted");

    TrackMinedBlock(pblock->GetHash());
    //komodo_broadcast(pblock,16);
    return true;
}

int32_t FOUND_BLOCK,KOMODO_MAYBEMINED;
extern int32_t KOMODO_LASTMINED,KOMODO_INSYNC;
arith_uint256 HASHTarget,HASHTarget_POW;

// wait for peers to connect
void waitForPeers(const CChainParams &chainparams)
{
    if (chainparams.MiningRequiresPeers())
    {
        bool fvNodesEmpty;
        {
            boost::this_thread::interruption_point();
            LOCK(cs_vNodes);
            fvNodesEmpty = vNodes.empty();
        }
        if (fvNodesEmpty || IsNotInSync())
        {
            int loops = 0, blockDiff = 0, newDiff = 0;

            do {
                if (fvNodesEmpty)
                {
                    MilliSleep(1000 + rand() % 4000); // allow to interrupt
                    boost::this_thread::interruption_point();
                    LOCK(cs_vNodes);
                    fvNodesEmpty = vNodes.empty();
                    loops = 0;
                    blockDiff = 0;
                }
                if ((newDiff = IsNotInSync()) > 1)
                {
                    if (blockDiff != newDiff)
                    {
                        blockDiff = newDiff;
                    }
                    else
                    {
                        if (++loops <= 10)
                        {
                            MilliSleep(1000); // allow to interrupt
                        }
                        else break;
                    }
                }
            } while (fvNodesEmpty || IsNotInSync());
            MilliSleep(100 + rand() % 400); // allow to interrupt
        }
    }
}

#ifdef ENABLE_WALLET
CBlockIndex *get_chainactive(int32_t height)
{
    if ( chainActive.Tip() != 0 )
    {
        if ( height <= chainActive.Tip()->nHeight )
        {
            LOCK(cs_main);
            return(chainActive[height]);
        }
        // else LogPrintf("get_chainactive height %d > active.%d\n",height,chainActive.Tip()->nHeight);
    }
    //LogPrintf("get_chainactive null chainActive.Tip() height %d\n",height);
    return(0);
}
#endif

int32_t gotinvalid;

bool check_tromp_solution(equi &eq, std::function<bool(std::vector<unsigned char>)> validBlock)
{
    // Convert solution indices to byte array (decompress) and pass it to validBlock method.
    for (size_t s = 0; s < std::min(MAXSOLS, eq.nsols); s++) 
    {
        LogPrint("pow", "Checking solution %d\n", s+1);
        std::vector<eh_index> index_vector(PROOFSIZE);
        for (size_t i = 0; i < PROOFSIZE; i++) {
            index_vector[i] = eq.sols[s][i];
        }
        std::vector<unsigned char> sol_char = GetMinimalFromIndices(index_vector, DIGITBITS);

        if (validBlock(sol_char)) {
            // If we find a POW solution, do not try other solutions
            // because they become invalid as we created a new block in blockchain.
            return true;
        }
    }
    return false;
}

#ifdef ENABLE_WALLET
void static BitcoinMiner(CWallet *pwallet)
#else
void static BitcoinMiner()
#endif
{
    LogPrintf("KomodoMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("komodo-miner");
    const CChainParams& chainparams = Params();

#ifdef ENABLE_WALLET
    // Each thread has its own key
    CReserveKey reservekey(pwallet);
#endif

    // Each thread has its own counter
    unsigned int nExtraNonce = 0;

    unsigned int n = chainparams.EquihashN();
    unsigned int k = chainparams.EquihashK();
    uint8_t *script; uint64_t total; int32_t i,j,gpucount=KOMODO_MAXGPUCOUNT,notaryid = -1;
    while ( (ASSETCHAIN_INIT == 0 || KOMODO_INITDONE == 0) )
    {
        //sleep(1);
        boost::this_thread::sleep_for(boost::chrono::seconds(1)); // allow to interrupt

        if ( komodo_baseid(chainName.symbol().c_str()) < 0 )
            break;
    }
    if ( chainName.isKMD() )
    {
        int newHeight;
        uint32_t timePast;
        {
            LOCK(cs_main);
            newHeight = chainActive.Height() + 1;
            timePast = (uint32_t)chainActive.Tip()->GetMedianTimePast();
        }
        komodo_chosennotary(&notaryid,newHeight,NOTARY_PUBKEY33,timePast);
    }
    if ( notaryid != My_notaryid )
        My_notaryid = notaryid;
    std::string solver;
    if ( ASSETCHAINS_NK[0] == 0 && ASSETCHAINS_NK[1] == 0 )
        solver = "tromp";
    else 
        solver = "default";
    assert(solver == "tromp" || solver == "default");
    LogPrint("pow", "Using Equihash solver \"%s\" with n = %u, k = %u\n", solver, n, k);
    if ( chainName.isKMD() )
        LogPrintf("notaryid.%d Mining.%s with %s\n",notaryid,chainName.symbol().c_str(),solver.c_str());
    std::mutex m_cs;
    bool cancelSolver = false;
    boost::signals2::connection c = uiInterface.NotifyBlockTip.connect(
//                                                                       [&m_cs, &cancelSolver](const uint256& hashNewTip) mutable {
                                                                         [&m_cs, &cancelSolver](bool, const CBlockIndex *) mutable {
                                                                           std::lock_guard<std::mutex> lock{m_cs};
                                                                           cancelSolver = true;
                                                                       }
                                                                       );
    miningTimer.start();

    try {
        if ( !chainName.isKMD() )
            LogPrintf("try %s Mining with %s\n",chainName.symbol().c_str(),solver.c_str());
        while (true)
        {
            if (chainparams.MiningRequiresPeers()) 
            {
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                miningTimer.stop();
                do {
                    bool fvNodesEmpty;
                    {
                        //LOCK(cs_vNodes);
                        fvNodesEmpty = vNodes.empty();
                    }
                    if (!fvNodesEmpty )//&& !IsInitialBlockDownload())
                        break;
                    MilliSleep(15000);
                    //LogPrintf("fvNodesEmpty %d IsInitialBlockDownload(%s) %d\n",(int32_t)fvNodesEmpty,chainName.symbol().c_str(),(int32_t)IsInitialBlockDownload());

                } while (true);
                miningTimer.start();
            }
            //
            // Create new block
            //
            unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
            CBlockIndex* pindexPrev = nullptr;
            {
                LOCK(cs_main);
                pindexPrev = chainActive.Tip();
            }

            // If we don't have a valid chain tip to work from, wait and try again.
            if (pindexPrev == nullptr) {
                MilliSleep(1000);
                continue;
            }

            if ( Mining_height != pindexPrev->nHeight+1 )
            {
                Mining_height = pindexPrev->nHeight+1;
                Mining_start = (uint32_t)time(NULL);
            }

#ifdef ENABLE_WALLET
            // notaries always default to staking
            CBlockTemplate *ptr = CreateNewBlockWithKey(reservekey, pindexPrev->nHeight+1, gpucount, ASSETCHAINS_STAKED != 0 && KOMODO_MININGTHREADS == 0);
#else
            CBlockTemplate *ptr = CreateNewBlockWithKey();
#endif
            if ( ptr == 0 )
            {
                if ( 0 && !GetBoolArg("-gen",false))
                {
                    miningTimer.stop();
                    c.disconnect();
                    LogPrintf("KomodoMiner terminated\n");
                    return;
                }
                static uint32_t counter;
                if ( counter++ < 10 && ASSETCHAINS_STAKED == 0 )
                    LogPrintf("created illegal block, retry\n");
                //sleep(1);
                boost::this_thread::sleep_for(boost::chrono::seconds(1)); // allow to interrupt
                continue;
            }
            //LogPrintf("get template\n");
            unique_ptr<CBlockTemplate> pblocktemplate(ptr);
            if (!pblocktemplate.get())
            {
                if (GetArg("-mineraddress", "").empty()) {
                    LogPrintf("Error in KomodoMiner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                } else {
                    // Should never reach here, because -mineraddress validity is checked in init.cpp
                    LogPrintf("Error in KomodoMiner: Invalid -mineraddress\n");
                }
                return;
            }
            CBlock *pblock = &pblocktemplate->block;
            if ( !chainName.isKMD() )
            {
                if ( ASSETCHAINS_REWARD[0] == 0 && !ASSETCHAINS_LASTERA )
                {
                    if ( pblock->vtx.size() == 1 && pblock->vtx[0].vout.size() == 1 && Mining_height > ASSETCHAINS_MINHEIGHT )
                    {
                        static uint32_t counter;
                        if ( counter++ < 10 )
                            LogPrintf("skip generating %s on-demand block, no tx avail\n",chainName.symbol().c_str());
                        //sleep(10);
                        boost::this_thread::sleep_for(boost::chrono::seconds(10)); // allow to interrupt

                        continue;
                    } else LogPrintf("%s vouts.%d mining.%d vs %d\n",chainName.symbol().c_str(),(int32_t)pblock->vtx[0].vout.size(),Mining_height,ASSETCHAINS_MINHEIGHT);
                }
            }
            // We cant increment nonce for proof transactions, as it modifes the coinbase, meaning CreateBlock must be called again to get a new valid proof to pass validation. 
            if ( (chainName.isKMD() && notaryid >= 0 && Mining_height > nDecemberHardforkHeight ) || (ASSETCHAINS_STAKED != 0 && komodo_newStakerActive(Mining_height, pblock->nTime) != 0) ) //December 2019 hardfork
                nExtraNonce = 0;
            IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);
            //LogPrintf("Running KomodoMiner.%s with %u transactions in block\n",solver.c_str(),(int32_t)pblock->vtx.size());
            LogPrintf("Running KomodoMiner.%s with %u transactions in block (%u bytes)\n",solver.c_str(),pblock->vtx.size(),::GetSerializeSize(*pblock,SER_NETWORK,PROTOCOL_VERSION));
            //
            // Search
            //
            uint8_t pubkeys[66][33]; arith_uint256 bnMaxPoSdiff; uint32_t blocktimes[66]; int mids[256],nonzpkeys,i,j,externalflag; uint32_t savebits; int64_t nStart = GetTime();
            pblock->nBits         = GetNextWorkRequired(pindexPrev, pblock, Params().GetConsensus());
            savebits = pblock->nBits;
            HASHTarget = arith_uint256().SetCompact(savebits);
            if ( chainName.isKMD() && notaryid >= 0 )
            {
                j = 65;
                if ( (Mining_height >= 235300 && Mining_height < 236000) || (Mining_height % KOMODO_ELECTION_GAP) > 64 || (Mining_height % KOMODO_ELECTION_GAP) == 0 || Mining_height > 1000000 )
                {
                    int32_t dispflag = 1; // TODO: set this back to 0 when finished testing.
                    if ( notaryid <= 3 || notaryid == 32 || (notaryid >= 43 && notaryid <= 45) || notaryid == 51 || notaryid == 52 || notaryid == 56 || notaryid == 57 )
                        dispflag = 1;
                    komodo_eligiblenotary(pubkeys,mids,blocktimes,&nonzpkeys,Mining_height);
                    if ( nonzpkeys > 0 )
                    {
                        for (i=0; i<33; i++)
                            if( pubkeys[0][i] != 0 )
                                break;
                        if ( i == 33 )
                            externalflag = 1;
                        else externalflag = 0;
                        if ( IS_KOMODO_NOTARY )
                        {
                            for (i=1; i<66; i++)
                                if ( memcmp(pubkeys[i],pubkeys[0],33) == 0 )
                                    break;
                            if ( externalflag == 0 && i != 66 && mids[i] >= 0 )
                                LogPrintf("VIOLATION at %d, notaryid.%d\n",i,mids[i]);
                            for (j=gpucount=0; j<65; j++)
                            {
                                if ( dispflag != 0 )
                                {
                                    if ( mids[j] >= 0 )
                                    {
                                        if ( mids[j] == notaryid )
                                            LogPrintf("--<%d>-- ",mids[j]);
                                        else
                                            LogPrintf("%d ",mids[j]);
                                    } else LogPrintf("GPU ");
                                }
                                if ( mids[j] == -1 )
                                    gpucount++;
                            }
                            if ( dispflag != 0 )
                                LogPrintf(" <- prev minerids from ht.%d notary.%d gpucount.%d %.2f%% t.%u\n",pindexPrev->nHeight,notaryid,gpucount,100.*(double)gpucount/j,(uint32_t)time(NULL));
                        }
                        for (j=0; j<65; j++)
                            if ( mids[j] == notaryid )
                                break;
                        if ( j == 65 )
                            KOMODO_LASTMINED = 0;
                    } else LogPrintf("ht.%i all NN are elegible\n",Mining_height); //else LogPrintf("no nonz pubkeys\n");

                    if ((Mining_height >= 235300 && Mining_height < 236000) ||
                        (j == 65 && Mining_height > KOMODO_MAYBEMINED + 1 && Mining_height > KOMODO_LASTMINED + 64))
                    {
                        HASHTarget = arith_uint256().SetCompact(KOMODO_MINDIFF_NBITS);
                        LogPrintf("I am the chosen one for %s ht.%d\n", chainName.symbol().c_str(), pindexPrev->nHeight + 1);
                    }
                    else
                        LogPrintf("duplicate at j.%d\n", j);

                    /* check if hf22 rule can be applied */
                    const Consensus::Params &params = chainparams.GetConsensus();
                    if (params.nHF22Height != boost::none)
                    {
                        const uint32_t nHeightAfterGAPSecondBlockAllowed = params.nHF22Height.get();
                        const uint32_t nMaxGAPAllowed = params.nMaxFutureBlockTime + 1;
                        const uint32_t tiptime = pindexPrev->GetBlockTime();

                        if (Mining_height > nHeightAfterGAPSecondBlockAllowed)
                        {
                            const uint32_t &blocktime = pblock->nTime;
                            if (blocktime >= tiptime + nMaxGAPAllowed && tiptime == blocktimes[1])
                            {
                                // already assumed notaryid >= 0 and chain is KMD
                                LogPrint("hfnet", "%s[%d]: time.(%lu >= %lu), notaryid.%ld, ht.%ld\n", __func__, __LINE__, blocktime, tiptime, notaryid, Mining_height);

                                /* build priority list */
                                std::vector<int32_t> vPriorityList(64);
                                // fill the priority list by notaries numbers, 0..63
                                int id = 0;
                                std::generate(vPriorityList.begin(), vPriorityList.end(), [&id] { return id++; }); // std::iota
                                // move the notaries participated in last 65 to the end of priority list
                                std::vector<int32_t>::iterator it;
                                for (size_t i = sizeof(mids) / sizeof(mids[0]) - 1; i > 0; --i)
                                { // ! mids[0] is not included
                                    if (mids[i] != -1)
                                    {
                                        it = std::find(vPriorityList.begin(), vPriorityList.end(), mids[i]);
                                        if (it != vPriorityList.end() && std::next(it) != vPriorityList.end())
                                        {
                                            std::rotate(it, std::next(it), vPriorityList.end());
                                        }
                                    }
                                }

                                if (isSecondBlockAllowed(notaryid, blocktime, tiptime + nMaxGAPAllowed, params.nHF22NotariesPriorityRotateDelta, vPriorityList))
                                {
                                    HASHTarget = arith_uint256().SetCompact(KOMODO_MINDIFF_NBITS);
                                    LogPrint("hfnet", "%s[%d]: notaryid.%ld, ht.%ld --> allowed to mine mindiff\n", __func__, __LINE__, notaryid, Mining_height);
                                }
                            }
                        }
                    } // hf22 rule check
                }
                else
                    Mining_start = 0;
            } else Mining_start = 0;

            if ( ASSETCHAINS_STAKED > 0 )
            {
                int32_t percPoS,z; bool fNegative,fOverflow;
                HASHTarget_POW = komodo_PoWtarget(&percPoS,HASHTarget,Mining_height,ASSETCHAINS_STAKED,komodo_newStakerActive(Mining_height, pblock->nTime));
                HASHTarget.SetCompact(KOMODO_MINDIFF_NBITS,&fNegative,&fOverflow);
                if ( ASSETCHAINS_STAKED < 100 )
                    LogPrintf("Block %d : PoS %d%% vs target %d%% \n",Mining_height,percPoS,(int32_t)ASSETCHAINS_STAKED);
            }

            gotinvalid = 0;
            while (true)
            {
                //LogPrintf("gotinvalid.%d\n",gotinvalid);
                if ( gotinvalid != 0 )
                    break;

                // Hash state

                crypto_generichash_blake2b_state state;
                EhInitialiseState(n, k, state);
                // I = the block header minus nonce and solution.
                CEquihashInput I{*pblock};
                CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                ss << I;
                // H(I||...
                crypto_generichash_blake2b_update(&state, (unsigned char*)&ss[0], ss.size());
                // H(I||V||...
                crypto_generichash_blake2b_state curr_state;
                curr_state = state;
                crypto_generichash_blake2b_update(&curr_state,pblock->nNonce.begin(),pblock->nNonce.size());
                // (x_1, x_2, ...) = A(I, V, n, k)
                LogPrint("pow", "Running Equihash solver \"%s\" with nNonce = %s\n",solver, pblock->nNonce.ToString());
                arith_uint256 hashTarget,hashTarget_POW = HASHTarget_POW;
                if ( KOMODO_MININGTHREADS > 0 && ASSETCHAINS_STAKED > 0 && ASSETCHAINS_STAKED < 100 && Mining_height > 10 )
                    hashTarget = HASHTarget_POW;
                //else if ( ASSETCHAINS_ADAPTIVEPOW > 0 )
                //    hashTarget = HASHTarget_POW;
                else hashTarget = HASHTarget;
                std::function<bool(std::vector<unsigned char>)> validBlock =
#ifdef ENABLE_WALLET
                [&pblock, &hashTarget, &pwallet, &reservekey, &m_cs, &cancelSolver, &chainparams, &hashTarget_POW]
#else
                [&pblock, &hashTarget, &m_cs, &cancelSolver, &chainparams, &hashTarget_POW]
#endif
                (std::vector<unsigned char> soln) {
                    int32_t z; arith_uint256 h; CBlock B;
                    // Write the solution to the hash and compute the result.
                    LogPrint("pow", "- Checking solution against target\n");
                    pblock->nSolution = soln;
                    solutionTargetChecks.increment();
                    B = *pblock;
                    h = UintToArith256(B.GetHash());
                    if ( h > hashTarget )
                    {
                        return false;
                    }
                    if ( IS_KOMODO_NOTARY && B.nTime > GetTime() )
                    {
                        while ( GetTime() < B.nTime-2 )
                        {
                            boost::this_thread::sleep_for(boost::chrono::seconds(1)); // allow to interrupt
                            CBlockIndex *tip = nullptr;

                            {
                                LOCK(cs_main);
                                tip = chainActive.Tip();
                            }
                            if ( tip->nHeight >= Mining_height )
                            {
                                LogPrintf("new block arrived\n");
                                return(false);
                            }
                        }
                    }
                    if ( ASSETCHAINS_STAKED == 0 )
                    {
                        if ( IS_KOMODO_NOTARY )
                        {
                            int32_t r;
                            if ( (r= ((Mining_height + NOTARY_PUBKEY33[16]) % 64) / 8) > 0 )
                                MilliSleep((rand() % (r * 1000)) + 1000); // allow to interrupt
                        }
                    }
                    else
                    {
                        if ( KOMODO_MININGTHREADS == 0 ) // we are staking 
                        {
                            // Need to rebuild block if the found solution for PoS, meets POW target, otherwise it will be rejected. 
                            if ( ASSETCHAINS_STAKED < 100 && komodo_newStakerActive(Mining_height,pblock->nTime) != 0 && h < hashTarget_POW )
                            {
                                LogPrintf( "[%s:%d] PoS block.%u meets POW_Target.%u building new block\n", chainName.symbol().c_str(), Mining_height, h.GetCompact(), hashTarget_POW.GetCompact());
                                return(false);
                            }
                            if ( komodo_waituntilelegible(B.nTime, Mining_height, ASSETCHAINS_STAKED_BLOCK_FUTURE_MAX) == 0 )
                                return(false);
                        }
                        uint256 tmp = B.GetHash();
                        LogPrintf("[%s:%d] mined block ",chainName.symbol().c_str(),Mining_height);
                        int32_t z; for (z=31; z>=0; z--)
                            LogPrintf("%02x",((uint8_t *)&tmp)[z]);
                        LogPrintf( "\n");
                    }
                    bool blockValid;
                    {
                        LOCK(cs_main);
                        CValidationState state;
                        blockValid = TestBlockValidity(state, B, chainActive.Tip(), true, false);
                    }
                    if ( !blockValid )
                    {
                        h = UintToArith256(B.GetHash());
                        gotinvalid = 1;
                        return(false);
                    }

                    // Found a solution
                    SetThreadPriority(THREAD_PRIORITY_NORMAL);
                    LogPrintf("KomodoMiner:\n");
                    LogPrintf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", B.GetHash().GetHex(), HASHTarget.GetHex());
#ifdef ENABLE_WALLET
                    if (ProcessBlockFound(&B, *pwallet, reservekey)) {
#else
                        if (ProcessBlockFound(&B)) {
#endif
                            // Ignore chain updates caused by us
                            std::lock_guard<std::mutex> lock{m_cs};
                            cancelSolver = false;
                        }

                        SetThreadPriority(THREAD_PRIORITY_LOWEST);
                        // In regression test mode, stop mining after a block is found.
                        if (chainparams.MineBlocksOnDemand()) {
                            // Increment here because throwing skips the call below
                            ehSolverRuns.increment();
                            throw boost::thread_interrupted();
                        }
                        return true;
                    };
                    std::function<bool(EhSolverCancelCheck)> cancelled = [&m_cs, &cancelSolver](EhSolverCancelCheck pos) {
                        std::lock_guard<std::mutex> lock{m_cs};
                        return cancelSolver;
                    };
                    // TODO: factor this out into a function with the same API for each solver.
                    if (solver == "tromp" ) { //&& notaryid >= 0 ) {
                        // Create solver and initialize it.
                        equi eq(1);
                        eq.setstate(&curr_state);

                        // Initialization done, start algo driver.
                        eq.digit0(0);
                        eq.xfull = eq.bfull = eq.hfull = 0;
                        eq.showbsizes(0);
                        for (u32 r = 1; r < WK; r++) {
                            (r&1) ? eq.digitodd(r, 0) : eq.digiteven(r, 0);
                            eq.xfull = eq.bfull = eq.hfull = 0;
                            eq.showbsizes(r);
                        }
                        eq.digitK(0);
                        ehSolverRuns.increment();

                        check_tromp_solution(eq, validBlock);
                        
                    } else {
                        try {
                            // If we find a valid block, we rebuild
                            bool found = EhOptimisedSolve(n, k, curr_state, validBlock, cancelled);
                            ehSolverRuns.increment();
                            if (found) {
                                break;
                            }
                        } catch (EhSolverCancelledException&) {
                            LogPrint("pow", "Equihash solver cancelled\n");
                            std::lock_guard<std::mutex> lock{m_cs};
                            cancelSolver = false;
                        }
                    }

                    // Check for stop or if block needs to be rebuilt
                    boost::this_thread::interruption_point();
                    // Regtest mode doesn't require peers
                    /*if ( FOUND_BLOCK != 0 )
                    {
                        FOUND_BLOCK = 0;
                        LogPrintf("FOUND_BLOCK!\n");
                        //sleep(2000);
                    } */
                    if (vNodes.empty() && chainparams.MiningRequiresPeers())
                    {
                        if ( chainName.isKMD() || Mining_height > ASSETCHAINS_MINHEIGHT )
                        {
                            LogPrintf("no nodes, break\n");
                            break;
                        }
                    }
                    if ((UintToArith256(pblock->nNonce) & 0xffff) == 0xffff)
                    {
                        //if ( 0 && ASSETCHAINS_SYMBOL[0] != 0 )
                        LogPrintf("0xffff, break\n");
                        break;
                    }
                    if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                    {
                        break;
                    }
                    CBlockIndex *tip = nullptr;
                    {
                        LOCK(cs_main);
                        tip = chainActive.Tip();
                    }
                    if ( pindexPrev != tip )
                    {
                        break;
                    }
                    // Update nNonce and nTime
                    pblock->nNonce = ArithToUint256(UintToArith256(pblock->nNonce) + 1);
                    pblock->nBits = savebits;
                    if ( ASSETCHAINS_ADAPTIVEPOW > 0 )
                    {
                        UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
                        HASHTarget.SetCompact(pblock->nBits);
                        hashTarget = HASHTarget;
                        savebits = pblock->nBits;
                    }
                }
            }
        }
        catch (const boost::thread_interrupted&)
        {
            miningTimer.stop();
            c.disconnect();
            LogPrintf("KomodoMiner terminated\n");
            throw;
        }
        catch (const std::runtime_error &e)
        {
            miningTimer.stop();
            c.disconnect();
            LogPrintf("KomodoMiner runtime error: %s\n", e.what());
            return;
        }
        miningTimer.stop();
        c.disconnect();
    }

#ifdef ENABLE_WALLET
    void GenerateBitcoins(bool fGenerate, CWallet* pwallet, int nThreads)
#else
    void GenerateBitcoins(bool fGenerate, int nThreads)
#endif
    {
        static boost::thread_group* minerThreads = NULL;

        if (nThreads < 0)
            nThreads = GetNumCores();

        if (minerThreads != NULL)
        {
            minerThreads->interrupt_all();
            minerThreads->join_all(); // prevent thread overlapping (https://github.com/zcash/zcash/commit/a17646b1fd0ce170bdb48399f3433ca94041af73)
            delete minerThreads;
            minerThreads = NULL;
        }

        //LogPrintf("nThreads.%d fGenerate.%d\n",(int32_t)nThreads,fGenerate);
        if ( ASSETCHAINS_STAKED > 0 && nThreads == 0 && fGenerate )
        {
            if ( pwallet != NULL )
                nThreads = 1;
            else
                return;
        }

        if (nThreads == 0 || !fGenerate)
            return;

        minerThreads = new boost::thread_group();

        for (int i = 0; i < nThreads; i++) {

#ifdef ENABLE_WALLET
            if ( ASSETCHAINS_ALGO == ASSETCHAINS_EQUIHASH )
                minerThreads->create_thread(boost::bind(&BitcoinMiner, pwallet));
#else
            if (ASSETCHAINS_ALGO == ASSETCHAINS_EQUIHASH )
                minerThreads->create_thread(&BitcoinMiner);
#endif
        }
    }

#endif // ENABLE_MINING

/****
 * This should only be called from a unit test, as the equihash code
 * can only be included once (no separation of implementation from declaration).
 * This verifies that std::min(MAXSOLS, eq.nsols) prevents a SIGSEGV.
 */
bool test_tromp_equihash()
{
    // get the sols to be less than nsols

    // create a context
    CBlock block;
    const CChainParams& params = Params();

    crypto_generichash_blake2b_state state;
    EhInitialiseState(params.EquihashN(), params.EquihashK(), state);
    // I = the block header minus nonce and solution.
    CEquihashInput I{block};
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << I;
    // H(I||...
    crypto_generichash_blake2b_update(&state, (unsigned char*)&ss[0], ss.size());
    // H(I||V||...
    crypto_generichash_blake2b_state curr_state;
    curr_state = state;
    crypto_generichash_blake2b_update(&state,block.nNonce.begin(),block.nNonce.size());

    // Create solver and initialize it.
    equi eq(1);
    eq.setstate(&curr_state);

    // Initialization done, start algo driver.
    eq.digit0(0);
    eq.xfull = eq.bfull = eq.hfull = 0;
    eq.showbsizes(0);
    for (u32 r = 1; r < WK; r++) {
        (r&1) ? eq.digitodd(r, 0) : eq.digiteven(r, 0);
        eq.xfull = eq.bfull = eq.hfull = 0;
        eq.showbsizes(r);
    }
    eq.digitK(0);

    // force nsols to be more than MAXSOLS (8)
    while (eq.nsols <= MAXSOLS * 800)
    {
        tree t(1);
        eq.candidate(t);
    }

    auto func = [](std::vector<unsigned char>) { return false; };   

    // this used to throw a SIGSEGV
    return check_tromp_solution(eq, func);
}
