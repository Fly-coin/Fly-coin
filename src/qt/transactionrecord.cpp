#include "transactionrecord.h"

#include "wallet.h"
#include "base58.h"
#include "txdb.h"

/* Return positive answer if transaction should be shown in list.
 */
bool TransactionRecord::showTransaction(const CWalletTx &wtx)
{
    if (wtx.IsCoinBase())
    {
        // Ensures we show generated coins / mined transactions at depth 1
        if (!wtx.IsInMainChain())
        {
            return false;
        }
    }
    return true;
}

/*
 * Decompose CWallet transaction to model transaction records.
 */
QList<TransactionRecord> TransactionRecord::decomposeTransaction(const CWallet *wallet, const CWalletTx &wtx)
{
    QList<TransactionRecord> parts;
    int64_t nTime = wtx.GetTxTime();
    int64_t nCredit = wtx.GetCredit(true);
    int64_t nDebit = wtx.GetDebit();
    int64_t nNet = nCredit - nDebit;
    uint256 hash = wtx.GetHash();
    std::map<std::string, std::string> mapValue = wtx.mapValue;

    if (wtx.IsCoinStake())
    {
        CTxDestination address;
		if (!ExtractDestination(wtx.vout[1].scriptPubKey, address))
			return parts;
		
		if(!IsMine(*wallet, address)) //if the address is not yours then it means you have a tx sent to you in someone elses coinstake tx
		{
			for(unsigned int i = 0; i < wtx.vout.size(); i++)
			{
				if(i == 0)
					continue; // first tx is blank
				CTxDestination outAddress;
				if(ExtractDestination(wtx.vout[i].scriptPubKey, outAddress))
				{
					if(IsMine(*wallet, outAddress))
					{
						TransactionRecord txrMultiSendRec = TransactionRecord(hash, nTime, TransactionRecord::RecvWithAddress, CBitcoinAddress(outAddress).ToString(), wtx.vout[i].nValue, 0);
						parts.append(txrMultiSendRec);
					}
				}
			}
		}
		else
		{
			TransactionRecord txrCoinStake = TransactionRecord(hash, nTime, TransactionRecord::StakeMint, CBitcoinAddress(address).ToString(), -nDebit, wtx.GetValueOut());
			// Stake generation
			
			
			// Find the block the tx is in
			CBlockIndex* pindex = NULL;
			std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(wtx.hashBlock);
			if (mi != mapBlockIndex.end())
				pindex = (*mi).second;
			uint256 prevHash = 0;
			if(pindex->pprev)
				prevHash = pindex->pprev->GetBlockHash();
			
			uint64_t nCoinAge;
			CTxDB txdb("r");
			if (!wtx.GetCoinAge(txdb, nCoinAge))
				return parts;
			int64_t nRewardMultiplier = 1;
			int64_t nReward = GetProofOfStakeReward(nCoinAge, pindex->nBits, wtx.nTime,nDebit - wtx.GetValueOut(), nDebit, prevHash, nRewardMultiplier);
			int64_t nBaseReward = nReward / nRewardMultiplier;
			if(nRewardMultiplier > 1)
			{
				TransactionRecord txrCoinStakeBonus = TransactionRecord(hash, nTime, TransactionRecord::StakeMintBonus, CBitcoinAddress(address).ToString(), nReward - nBaseReward, wtx.GetValueOut());
				// Stake generation
				txrCoinStake.credit = nBaseReward;
				parts.append(txrCoinStake);
				parts.append(txrCoinStakeBonus);
			}
			else
				parts.append(txrCoinStake);
			
			//if some of your outputs went to another address we will make them as a sendtoaddress tx
			for(unsigned int i = 0; i < wtx.vout.size(); i++)
			{
				if(i == 0)
					continue; //first tx is blank
				CTxDestination outAddress;
				if(ExtractDestination(wtx.vout[i].scriptPubKey, outAddress))
				{
					if(CBitcoinAddress(outAddress).ToString() != CBitcoinAddress(address).ToString())
					{
						TransactionRecord txrCoinStakeMultiSend = TransactionRecord(hash, nTime, TransactionRecord::SendToAddress, CBitcoinAddress(outAddress).ToString(), wtx.vout[i].nValue * -1, 0);
						parts.append(txrCoinStakeMultiSend);
					}
				}
			}
		}
    }
    else if (nNet > 0 || wtx.IsCoinBase())
    {
        //
        // Credit
        //
        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
        {
            if(wallet->IsMine(txout))
            {
                TransactionRecord sub(hash, nTime);
                CTxDestination address;
                sub.idx = parts.size(); // sequence number
                sub.credit = txout.nValue;
                if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*wallet, address))
                {
                    // Received by Bitcoin Address
                    sub.type = TransactionRecord::RecvWithAddress;
                    sub.address = CBitcoinAddress(address).ToString();
                }
                else
                {
                    // Received by IP connection (deprecated features), or a multisignature or other non-simple transaction
                    sub.type = TransactionRecord::RecvFromOther;
                    sub.address = mapValue["from"];
                }
                if (wtx.IsCoinBase())
                {
                    // Generated
                    sub.type = TransactionRecord::Generated;
                }

                parts.append(sub);
            }
        }
    }
    else
    {
        bool fAllFromMe = true;
        BOOST_FOREACH(const CTxIn& txin, wtx.vin)
            fAllFromMe = fAllFromMe && wallet->IsMine(txin);

        bool fAllToMe = true;
        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
            fAllToMe = fAllToMe && wallet->IsMine(txout);

        if (fAllFromMe && fAllToMe)
        {
            // Payment to self
            int64_t nChange = wtx.GetChange();	
			TransactionRecord sub(hash, nTime);
			sub.type = TransactionRecord::SendToSelf;
			sub.credit = nCredit - nChange;
			sub.debit =  -(nDebit - nChange);		
			CTxDestination address;
			if (ExtractDestination(wtx.vout[0].scriptPubKey, address))
				 sub.address = CBitcoinAddress(address).ToString();
			parts.append(sub);
        }
        else if (fAllFromMe)
        {
            //
            // Debit
            //
            int64_t nTxFee = nDebit - wtx.GetValueOut();

            for (unsigned int nOut = 0; nOut < wtx.vout.size(); nOut++)
            {
                const CTxOut& txout = wtx.vout[nOut];
                TransactionRecord sub(hash, nTime);
                sub.idx = parts.size();

                if(wallet->IsMine(txout))
                {
                    // Ignore parts sent to self, as this is usually the change
                    // from a transaction sent back to our own address.
                    continue;
                }

                CTxDestination address;
                if (ExtractDestination(txout.scriptPubKey, address))
                {
                    // Sent to Bitcoin Address
                    sub.type = TransactionRecord::SendToAddress;
                    sub.address = CBitcoinAddress(address).ToString();
                }
                else
                {
                    // Sent to IP, or other non-address transaction like OP_EVAL
                    sub.type = TransactionRecord::SendToOther;
                    sub.address = mapValue["to"];
                }

                int64_t nValue = txout.nValue;
                /* Add fee to first output */
                if (nTxFee > 0)
                {
                    nValue += nTxFee;
                    nTxFee = 0;
                }
                sub.debit = -nValue;

                parts.append(sub);
            }
        }
        else
        {
            //
            // Mixed debit transaction, can't break down payees
            //
            parts.append(TransactionRecord(hash, nTime, TransactionRecord::Other, "", nNet, 0));
        }
    }

    return parts;
}

void TransactionRecord::updateStatus(const CWalletTx &wtx)
{
    // Determine transaction status

    // Find the block the tx is in
    CBlockIndex* pindex = NULL;
    std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(wtx.hashBlock);
    if (mi != mapBlockIndex.end())
        pindex = (*mi).second;

    // Sort order, unrecorded transactions sort to the top
    status.sortKey = strprintf("%010d-%01d-%010u-%03d",
        (pindex ? pindex->nHeight : std::numeric_limits<int>::max()),
        (wtx.IsCoinBase() ? 1 : 0),
        wtx.nTimeReceived,
        idx);
    status.confirmed = wtx.IsConfirmed();
    status.depth = wtx.GetDepthInMainChain();
    status.cur_num_blocks = nBestHeight;

    if (!wtx.IsFinal())
    {
        if (wtx.nLockTime < LOCKTIME_THRESHOLD)
        {
            status.status = TransactionStatus::OpenUntilBlock;
            status.open_for = nBestHeight - wtx.nLockTime;
        }
        else
        {
            status.status = TransactionStatus::OpenUntilDate;
            status.open_for = wtx.nLockTime;
        }
    }
    else
    {
        if (GetAdjustedTime() - wtx.nTimeReceived > 2 * 60 && wtx.GetRequestCount() == 0)
        {
            status.status = TransactionStatus::Offline;
        }
        else if (status.depth < RecommendedNumConfirmations)
        {
            status.status = TransactionStatus::Unconfirmed;
        }
        else
        {
            status.status = TransactionStatus::HaveConfirmations;
        }
    }

    // For generated transactions, determine maturity
 if(type == TransactionRecord::Generated || type == TransactionRecord::StakeMint)
    {
        int64_t nCredit = wtx.GetCredit(true);
        if (nCredit == 0)
        {
            status.maturity = TransactionStatus::Immature;

            if (wtx.IsInMainChain())
            {
                status.matures_in = wtx.GetBlocksToMaturity();

                // Check if the block was requested by anyone
                if (GetAdjustedTime() - wtx.nTimeReceived > 2 * 60 && wtx.GetRequestCount() == 0)
                    status.maturity = TransactionStatus::MaturesWarning;
            }
            else
            {
                status.maturity = TransactionStatus::NotAccepted;
            }
        }
        else
        {
            status.maturity = TransactionStatus::Mature;
        }
    }
}

bool TransactionRecord::statusUpdateNeeded()
{
    return status.cur_num_blocks != nBestHeight;
}

std::string TransactionRecord::getTxID()
{
    return hash.ToString() + strprintf("-%03d", idx);
}

