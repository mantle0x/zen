// Copyright (c) 2016 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "asyncrpcoperation_sendmany.h"
#include "asyncrpcqueue.h"
#include "core_io.h"
#include "init.h"
#include "main.h"
#include "net.h"
#include "netbase.h"
#include "rpc/server.h"
#include "timedata.h"
#include "util.h"
#include "utilmoneystr.h"
#include "walletdb.h"
#include "script/interpreter.h"
#include "utiltime.h"
#include "rpc/protocol.h"
#include "zcash/IncrementalMerkleTree.hpp"
#include "sodium.h"
#include "miner.h"

#include <array>
#include <iostream>
#include <chrono>
#include <thread>
#include <string>

#include "paymentdisclosuredb.h"

using namespace libzcash;

int find_output(UniValue obj, int n) {
    UniValue outputMapValue = find_value(obj, "outputmap");
    if (!outputMapValue.isArray()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Missing outputmap for JoinSplit operation");
    }

    UniValue outputMap = outputMapValue.get_array();
    assert(outputMap.size() == ZC_NUM_JS_OUTPUTS);
    for (size_t i = 0; i < outputMap.size(); i++) {
        if (outputMap[i].get_int() == n) {
            return i;
        }
    }

    throw std::logic_error("n is not present in outputmap");
}

AsyncRPCOperation_sendmany::AsyncRPCOperation_sendmany(
        CMutableTransaction contextualTx,
        const std::string& fromAddress,
        std::vector<SendManyRecipient>&& tOutputs,
        std::vector<SendManyRecipient>&& zOutputs,
        int minDepth,
        CAmount fee,
        UniValue contextInfo,
        bool sendChangeToSource) :
        tx_(contextualTx), fromaddress_(fromAddress), t_outputs_(std::move(tOutputs)), z_outputs_(std::move(zOutputs)), mindepth_(minDepth), fee_(fee), contextinfo_(contextInfo), sendChangeToSource_(sendChangeToSource)
{
    assert(fee_ >= 0);

    if (minDepth < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Minconf cannot be negative");
    }

    if (fromAddress.size() == 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "From address parameter missing");
    }

    if (t_outputs_.size() == 0 && z_outputs_.size() == 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "No recipients");
    }

    fromtaddr_ = CBitcoinAddress(fromAddress);
    isfromtaddr_ = fromtaddr_.IsValid();
    isfromzaddr_ = false;

    if (!isfromtaddr_) {
        CZCPaymentAddress address(fromAddress);
        try {
            PaymentAddress addr = address.Get();

            // We don't need to lock on the wallet as spending key related methods are thread-safe
            SpendingKey key;
            if (!pwalletMain->GetSpendingKey(addr, key)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid from address, no spending key found for zaddr");
            }

            isfromzaddr_ = true;
            frompaymentaddress_ = addr;
            spendingkey_ = key;
        } catch (const std::runtime_error& e) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid from address");
        }
    }

    if (isfromzaddr_ && minDepth==0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Minconf cannot be zero when sending from zaddr");
    }

    // Log the context info i.e. the call parameters to z_sendmany
    if (LogAcceptCategory("zrpcunsafe")) {
        LogPrint("zrpcunsafe", "%s: z_sendmany initialized (params=%s)\n", getId(), contextInfo.write());
    } else {
        LogPrint("zrpc", "%s: z_sendmany initialized\n", getId());
    }


    // Enable payment disclosure if requested
    paymentDisclosureMode = fExperimentalMode && GetBoolArg("-paymentdisclosure", false);
}

AsyncRPCOperation_sendmany::~AsyncRPCOperation_sendmany() {
}

void AsyncRPCOperation_sendmany::main() {
    if (isCancelled())
        return;

    set_state(OperationStatus::EXECUTING);
    start_execution_clock();

    bool success = false;

#ifdef ENABLE_MINING
  #ifdef ENABLE_WALLET
    GenerateBitcoins(false, NULL, 0);
  #else
    GenerateBitcoins(false, 0);
  #endif
#endif

    try {
        success = main_impl();
    } catch (const UniValue& objError) {
        int code = find_value(objError, "code").get_int();
        std::string message = find_value(objError, "message").get_str();
        set_error_code(code);
        set_error_message(message);
    } catch (const runtime_error& e) {
        set_error_code(-1);
        set_error_message("runtime error: " + string(e.what()));
    } catch (const logic_error& e) {
        set_error_code(-1);
        set_error_message("logic error: " + string(e.what()));
    } catch (const exception& e) {
        set_error_code(-1);
        set_error_message("general exception: " + string(e.what()));
    } catch (...) {
        set_error_code(-2);
        set_error_message("unknown error");
    }

#ifdef ENABLE_MINING
  #ifdef ENABLE_WALLET
    GenerateBitcoins(GetBoolArg("-gen",false), pwalletMain, GetArg("-genproclimit", 1));
  #else
    GenerateBitcoins(GetBoolArg("-gen",false), GetArg("-genproclimit", 1));
  #endif
#endif

    stop_execution_clock();

    if (success) {
        set_state(OperationStatus::SUCCESS);
    } else {
        set_state(OperationStatus::FAILED);
    }

    std::string s = strprintf("%s: z_sendmany finished (status=%s", getId(), getStateAsString());
    if (success) {
        s += strprintf(", txid=%s)\n", tx_.GetHash().ToString());
    } else {
        s += strprintf(", error=%s)\n", getErrorMessage());
    }
    LogPrintf("%s",s);

    // !!! Payment disclosure START
    if (success && paymentDisclosureMode && paymentDisclosureData_.size()>0) {
        uint256 txidhash = tx_.GetHash();
        std::shared_ptr<PaymentDisclosureDB> db = PaymentDisclosureDB::sharedInstance();
        for (PaymentDisclosureKeyInfo p : paymentDisclosureData_) {
            p.first.hash = txidhash;
            if (!db->Put(p.first, p.second)) {
                LogPrint("paymentdisclosure", "%s: Payment Disclosure: Error writing entry to database for key %s\n", getId(), p.first.ToString());
            } else {
                LogPrint("paymentdisclosure", "%s: Payment Disclosure: Successfully added entry to database for key %s\n", getId(), p.first.ToString());
            }
        }
    }
    // !!! Payment disclosure END
}

// Notes:
// 1. #1159 Currently there is no limit set on the number of joinsplits, so size of tx could be invalid.
// 2. #1360 Note selection is not optimal
// 3. #1277 Spendable notes are not locked, so an operation running in parallel could also try to use them
bool AsyncRPCOperation_sendmany::main_impl() {

    assert(isfromtaddr_ != isfromzaddr_);

    bool isSingleZaddrOutput = (t_outputs_.size()==0 && z_outputs_.size()==1);
    bool isMultipleZaddrOutput = (t_outputs_.size()==0 && z_outputs_.size()>=1);
    bool isPureTaddrOnlyTx = (isfromtaddr_ && z_outputs_.size() == 0);
    CAmount minersFee = fee_;

    // When spending coinbase utxos, you can only specify a single zaddr as the change must go somewhere
    // and if there are multiple zaddrs, we don't know where to send it.
    if (isfromtaddr_) {
        if (isSingleZaddrOutput) {
            bool b = find_utxos(true);
            if (!b) {
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds, no UTXOs found for taddr from address.");
            }
        } else {
            bool b = find_utxos(false);
            if (!b) {
                if (isMultipleZaddrOutput) {
                    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Could not find any non-coinbase UTXOs to spend. Coinbase UTXOs can only be sent to a single zaddr recipient.");
                } else {
                    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Could not find any non-coinbase UTXOs to spend.");
                }
            }
        }
    }

    if (isfromzaddr_ && !find_unspent_notes()) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds, no unspent notes found for zaddr from address.");
    }

    CAmount t_inputs_total = 0;
    for (SendManyInputUTXO & t : t_inputs_) {
        t_inputs_total += std::get<2>(t);
    }

    CAmount z_inputs_total = 0;
    for (SendManyInputJSOP & t : z_inputs_) {
        z_inputs_total += std::get<2>(t);
    }

    CAmount t_outputs_total = 0;
    for (SendManyRecipient & t : t_outputs_) {
        t_outputs_total += std::get<1>(t);
    }

    CAmount z_outputs_total = 0;
    for (SendManyRecipient & t : z_outputs_) {
        z_outputs_total += std::get<1>(t);
    }

    CAmount sendAmount = z_outputs_total + t_outputs_total;
    CAmount targetAmount = sendAmount + minersFee;

    assert(!isfromtaddr_ || z_inputs_total == 0);
    assert(!isfromzaddr_ || t_inputs_total == 0);

    if (isfromtaddr_ && (t_inputs_total < targetAmount)) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
            strprintf("Insufficient transparent funds, have %s, need %s",
            FormatMoney(t_inputs_total), FormatMoney(targetAmount)));
    }

    if (isfromzaddr_ && (z_inputs_total < targetAmount)) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
            strprintf("Insufficient protected funds, have %s, need %s",
            FormatMoney(z_inputs_total), FormatMoney(targetAmount)));
    }

    // If from address is a taddr, select UTXOs to spend
    CAmount selectedUTXOAmount = 0;
    bool selectedUTXOCoinbase = false;
    if (isfromtaddr_) {
        // Get dust threshold
        CKey secret;
        secret.MakeNewKey(true);
        CScript scriptPubKey = GetScriptForDestination(secret.GetPubKey().GetID());
        CTxOut out(CAmount(1), scriptPubKey);
        CAmount dustThreshold = out.GetDustThreshold(minRelayTxFee);
        CAmount dustChange = -1;

        std::vector<SendManyInputUTXO> selectedTInputs;
        for (SendManyInputUTXO & t : t_inputs_) {
            bool b = std::get<3>(t);
            if (b) {
                selectedUTXOCoinbase = true;
            }
            selectedUTXOAmount += std::get<2>(t);
            selectedTInputs.push_back(t);
            if (selectedUTXOAmount >= targetAmount) {
                // Select another utxo if there is change less than the dust threshold.
                dustChange = selectedUTXOAmount - targetAmount;
                if (dustChange == 0 || dustChange >= dustThreshold) {
                    break;
                }
            }
        }

        // If there is transparent change, is it valid or is it dust?
        if (dustChange < dustThreshold && dustChange != 0) {
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                strprintf("Insufficient transparent funds, have %s, need %s more to avoid creating invalid change output %s (dust threshold is %s)",
                FormatMoney(t_inputs_total), FormatMoney(dustThreshold - dustChange), FormatMoney(dustChange), FormatMoney(dustThreshold)));
        }

        t_inputs_ = selectedTInputs;
        t_inputs_total = selectedUTXOAmount;

        // Check mempooltxinputlimit to avoid creating a transaction which the local mempool rejects
        size_t limit = (size_t)GetArg("-mempooltxinputlimit", 0);
        if (limit > 0) {
            size_t n = t_inputs_.size();
            if (n > limit) {
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Too many transparent inputs %zu > limit %zu", n, limit));
            }
        }

        // update the transaction with these inputs
        CMutableTransaction rawTx(tx_);
        for (SendManyInputUTXO & t : t_inputs_) {
            uint256 txid = std::get<0>(t);
            int vout = std::get<1>(t);
            CAmount amount = std::get<2>(t);
            CTxIn in(COutPoint(txid, vout));
            rawTx.vin.push_back(in);
        }
        tx_ = CTransaction(rawTx);
    }

    LogPrint((isfromtaddr_) ? "zrpc" : "zrpcunsafe", "%s: spending %s to send %s with fee %s\n",
            getId(), FormatMoney(targetAmount), FormatMoney(sendAmount), FormatMoney(minersFee));
    LogPrint("zrpc", "%s: transparent input: %s (to choose from)\n", getId(), FormatMoney(t_inputs_total));
    LogPrint("zrpcunsafe", "%s: private input: %s (to choose from)\n", getId(), FormatMoney(z_inputs_total));
    LogPrint("zrpc", "%s: transparent output: %s\n", getId(), FormatMoney(t_outputs_total));
    LogPrint("zrpcunsafe", "%s: private output: %s\n", getId(), FormatMoney(z_outputs_total));
    LogPrint("zrpc", "%s: fee: %s\n", getId(), FormatMoney(minersFee));

    /**
     * SCENARIO #1
     *
     * taddr -> taddrs
     *
     * There are no zaddrs or joinsplits involved.
     */
    if (isPureTaddrOnlyTx) {
        add_taddr_outputs_to_tx();

        CAmount funds = selectedUTXOAmount;
        CAmount fundsSpent = t_outputs_total + minersFee;
        CAmount change = funds - fundsSpent;

        if (change > 0) {
            add_taddr_change_output_to_tx(change, sendChangeToSource_);

            LogPrint("zrpc", "%s: transparent change in transaction output (amount=%s)\n",
                    getId(),
                    FormatMoney(change)
                    );
        }

        UniValue obj(UniValue::VOBJ);
        obj.pushKV("rawtxn", EncodeHexTx(tx_));
        sign_send_raw_transaction(obj);
        return true;
    }
    /**
     * END SCENARIO #1
     */


    // Prepare raw transaction to handle JoinSplits
    CMutableTransaction mtx(tx_);
    crypto_sign_keypair(joinSplitPubKey_.begin(), joinSplitPrivKey_);
    mtx.joinSplitPubKey = joinSplitPubKey_;
    tx_ = CTransaction(mtx);

    // Copy zinputs and zoutputs to more flexible containers
    std::deque<SendManyInputJSOP> zInputsDeque; // zInputsDeque stores minimum numbers of notes for target amount
    CAmount tmp = 0;
    for (auto o : z_inputs_) {
        zInputsDeque.push_back(o);
        tmp += std::get<2>(o);
        if (tmp >= targetAmount) {
            break;
        }
    }
    std::deque<SendManyRecipient> zOutputsDeque;
    for (auto o : z_outputs_) {
        zOutputsDeque.push_back(o);
    }

    // When spending notes, take a snapshot of note witnesses and anchors as the treestate will
    // change upon arrival of new blocks which contain joinsplit transactions.  This is likely
    // to happen as creating a chained joinsplit transaction can take longer than the block interval.
    if (z_inputs_.size() > 0) {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        for (auto t : z_inputs_) {
            JSOutPoint jso = std::get<0>(t);
            std::vector<JSOutPoint> vOutPoints = { jso };
            uint256 inputAnchor;
            std::vector<std::optional<ZCIncrementalWitness>> vInputWitnesses;
            pwalletMain->GetNoteWitnesses(vOutPoints, vInputWitnesses, inputAnchor);
            jsopWitnessAnchorMap[ jso.ToString() ] = WitnessAnchorData{ vInputWitnesses[0], inputAnchor };
        }
    }


    /**
     * SCENARIO #2
     *
     * taddr -> taddrs
     *       -> zaddrs
     *
     * Note: Consensus rule states that coinbase utxos can only be sent to a zaddr.
     *       Local wallet rule does not allow any change when sending coinbase utxos
     *       since there is currently no way to specify a change address and we don't
     *       want users accidentally sending excess funds to a recipient.
     */
    if (isfromtaddr_) {
        add_taddr_outputs_to_tx();

        CAmount funds = selectedUTXOAmount;
        CAmount fundsSpent = t_outputs_total + minersFee + z_outputs_total;
        CAmount change = funds - fundsSpent;

        if (change > 0) {
            if (selectedUTXOCoinbase) {
                assert(isSingleZaddrOutput);
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf(
                    "Change %s not allowed. When protecting coinbase funds, the wallet does not "
                    "allow any change as there is currently no way to specify a change address "
                    "in z_sendmany.", FormatMoney(change)));
            } else {
                add_taddr_change_output_to_tx(change, sendChangeToSource_);
                LogPrint("zrpc", "%s: transparent change in transaction output (amount=%s)\n",
                        getId(),
                        FormatMoney(change)
                        );
            }
        }

        // Create joinsplits, where each output represents a zaddr recipient.
        UniValue obj(UniValue::VOBJ);
        while (zOutputsDeque.size() > 0) {
            AsyncJoinSplitInfo info;
            info.vpub_old = 0;
            info.vpub_new = 0;
            int n = 0;
            while (n++<ZC_NUM_JS_OUTPUTS && zOutputsDeque.size() > 0) {
                SendManyRecipient smr = zOutputsDeque.front();
                std::string address = std::get<0>(smr);
                CAmount value = std::get<1>(smr);
                std::string hexMemo = std::get<2>(smr);
                zOutputsDeque.pop_front();

                PaymentAddress pa = CZCPaymentAddress(address).Get();
                JSOutput jso = JSOutput(pa, value);
                if (hexMemo.size() > 0) {
                    jso.memo = get_memo_from_hex_string(hexMemo);
                }
                info.vjsout.push_back(jso);

                // Funds are removed from the value pool and enter the private pool
                info.vpub_old += value;
            }
            obj = perform_joinsplit(info);
        }
        sign_send_raw_transaction(obj);
        return true;
    }
    /**
     * END SCENARIO #2
     */



    /**
     * SCENARIO #3
     *
     * zaddr -> taddrs
     *       -> zaddrs
     *
     * Send to zaddrs by chaining JoinSplits together and immediately consuming any change
     * Send to taddrs by creating dummy z outputs and accumulating value in a change note
     * which is used to set vpub_new in the last chained joinsplit.
     */
    UniValue obj(UniValue::VOBJ);
    CAmount jsChange = 0;   // this is updated after each joinsplit
    int changeOutputIndex = -1; // this is updated after each joinsplit if jsChange > 0
    bool vpubNewProcessed = false;  // updated when vpub_new for miner fee and taddr outputs is set in last joinsplit
    CAmount vpubNewTarget = minersFee;
    if (t_outputs_total > 0) {
        add_taddr_outputs_to_tx();
        vpubNewTarget += t_outputs_total;
    }

    // Keep track of treestate within this transaction
    boost::unordered_map<uint256, ZCIncrementalMerkleTree, CCoinsKeyHasher> intermediates;
    std::vector<uint256> previousCommitments;

    while (!vpubNewProcessed) {
        AsyncJoinSplitInfo info;
        info.vpub_old = 0;
        info.vpub_new = 0;

        CAmount jsInputValue = 0;
        uint256 jsAnchor;
        std::vector<std::optional<ZCIncrementalWitness>> witnesses;

        JSDescription prevJoinSplit;

        // Keep track of previous JoinSplit and its commitments
        if (tx_.GetVjoinsplit().size() > 0) {
            prevJoinSplit = tx_.GetVjoinsplit().back();
        }

        // If there is no change, the chain has terminated so we can reset the tracked treestate.
        if (jsChange==0 && tx_.GetVjoinsplit().size() > 0) {
            intermediates.clear();
            previousCommitments.clear();
        }

        //
        // Consume change as the first input of the JoinSplit.
        //
        if (jsChange > 0) {
            LOCK2(cs_main, pwalletMain->cs_wallet);

            // Update tree state with previous joinsplit
            ZCIncrementalMerkleTree tree;
            auto it = intermediates.find(prevJoinSplit.anchor);
            if (it != intermediates.end()) {
                tree = it->second;
            } else if (!pcoinsTip->GetAnchorAt(prevJoinSplit.anchor, tree)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Could not find previous JoinSplit anchor");
            }

            assert(changeOutputIndex != -1);
            std::optional<ZCIncrementalWitness> changeWitness;
            int n = 0;
            for (const uint256& commitment : prevJoinSplit.commitments) {
                tree.append(commitment);
                previousCommitments.push_back(commitment);
                if (!changeWitness && changeOutputIndex == n++) {
                    changeWitness = tree.witness();
                } else if (changeWitness) {
                    changeWitness.value().append(commitment);
                }
            }
            if (changeWitness) {
                    witnesses.push_back(changeWitness);
            }
            jsAnchor = tree.root();
            intermediates.insert(std::make_pair(tree.root(), tree));    // chained js are interstitial (found in between block boundaries)

            // Decrypt the change note's ciphertext to retrieve some data we need
            ZCNoteDecryption decryptor(spendingkey_.receiving_key());
            auto hSig = prevJoinSplit.h_sig(*pzcashParams, tx_.joinSplitPubKey);
            try {
                NotePlaintext plaintext = NotePlaintext::decrypt(
                        decryptor,
                        prevJoinSplit.ciphertexts[changeOutputIndex],
                        prevJoinSplit.ephemeralKey,
                        hSig,
                        (unsigned char) changeOutputIndex);

                Note note = plaintext.note(frompaymentaddress_);
                info.notes.push_back(note);

                jsInputValue += plaintext.value();

                LogPrint("zrpcunsafe", "%s: spending change (amount=%s)\n",
                    getId(),
                    FormatMoney(plaintext.value())
                    );

            } catch (const std::exception& e) {
                throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Error decrypting output note of previous JoinSplit: %s", e.what()));
            }
        }


        //
        // Consume spendable non-change notes
        //
        std::vector<Note> vInputNotes;
        std::vector<JSOutPoint> vOutPoints;
        std::vector<std::optional<ZCIncrementalWitness>> vInputWitnesses;
        uint256 inputAnchor;
        int numInputsNeeded = (jsChange>0) ? 1 : 0;
        while (numInputsNeeded++ < ZC_NUM_JS_INPUTS && zInputsDeque.size() > 0) {
            SendManyInputJSOP t = zInputsDeque.front();
            JSOutPoint jso = std::get<0>(t);
            Note note = std::get<1>(t);
            CAmount noteFunds = std::get<2>(t);
            zInputsDeque.pop_front();

            WitnessAnchorData wad = jsopWitnessAnchorMap[ jso.ToString() ];
            vInputWitnesses.push_back(wad.witness);
            if (inputAnchor.IsNull()) {
                inputAnchor = wad.anchor;
            } else if (inputAnchor != wad.anchor) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Selected input notes do not share the same anchor");
            }

            vOutPoints.push_back(jso);
            vInputNotes.push_back(note);

            jsInputValue += noteFunds;

            int wtxHeight = -1;
            int wtxDepth = -1;
            {
                LOCK2(cs_main, pwalletMain->cs_wallet);
                const CWalletTransactionBase& wtx = *(pwalletMain->getMapWallet().at(jso.hash));
                // Zero confirmaton notes belong to transactions which have not yet been mined
                if (mapBlockIndex.find(wtx.hashBlock) == mapBlockIndex.end()) {
                    throw JSONRPCError(RPC_WALLET_ERROR, strprintf("mapBlockIndex does not contain block hash %s", wtx.hashBlock.ToString()));
                }
                wtxHeight = mapBlockIndex[wtx.hashBlock]->nHeight;
                wtxDepth = wtx.GetDepthInMainChain();
            }
            LogPrint("zrpcunsafe", "%s: spending note (txid=%s, vjoinsplit=%d, ciphertext=%d, amount=%s, height=%d, confirmations=%d)\n",
                    getId(),
                    jso.hash.ToString().substr(0, 10),
                    jso.js,
                    int(jso.n), // uint8_t
                    FormatMoney(noteFunds),
                    wtxHeight,
                    wtxDepth
                    );
        }

        // Add history of previous commitments to witness
        if (vInputNotes.size() > 0) {

            if (vInputWitnesses.size()==0) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Could not find witness for note commitment");
            }

            for (auto & optionalWitness : vInputWitnesses) {
                if (!optionalWitness) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Witness for note commitment is null");
                }
                ZCIncrementalWitness w = *optionalWitness; // could use .get();
                if (jsChange > 0) {
                    for (const uint256& commitment : previousCommitments) {
                        w.append(commitment);
                    }
                    if (jsAnchor != w.root()) {
                        throw JSONRPCError(RPC_WALLET_ERROR, "Witness for spendable note does not have same anchor as change input");
                    }
                }
                witnesses.push_back(w);
            }

            // The jsAnchor is null if this JoinSplit is at the start of a new chain
            if (jsAnchor.IsNull()) {
                jsAnchor = inputAnchor;
            }

            // Add spendable notes as inputs
            std::copy(vInputNotes.begin(), vInputNotes.end(), std::back_inserter(info.notes));
        }

        // Find recipient to transfer funds to
        std::string address, hexMemo;
        CAmount value = 0;
        if (zOutputsDeque.size() > 0) {
            SendManyRecipient smr = zOutputsDeque.front();
            address = std::get<0>(smr);
            value = std::get<1>(smr);
            hexMemo = std::get<2>(smr);
            zOutputsDeque.pop_front();
        }

        // Reset change
        jsChange = 0;
        CAmount outAmount = value;

        // Set vpub_new in the last joinsplit (when there are no more notes to spend or zaddr outputs to satisfy)
        if (zOutputsDeque.size() == 0 && zInputsDeque.size() == 0) {
            assert(!vpubNewProcessed);
            if (jsInputValue < vpubNewTarget) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                    strprintf("Insufficient funds for vpub_new %s (miners fee %s, taddr outputs %s)",
                    FormatMoney(vpubNewTarget), FormatMoney(minersFee), FormatMoney(t_outputs_total)));
            }
            outAmount += vpubNewTarget;
            info.vpub_new += vpubNewTarget; // funds flowing back to public pool
            vpubNewProcessed = true;
            jsChange = jsInputValue - outAmount;
            assert(jsChange >= 0);
        }
        else {
            // This is not the last joinsplit, so compute change and any amount still due to the recipient
            if (jsInputValue > outAmount) {
                jsChange = jsInputValue - outAmount;
            } else if (outAmount > jsInputValue) {
                // Any amount due is owed to the recipient.  Let the miners fee get paid first.
                CAmount due = outAmount - jsInputValue;
                SendManyRecipient r = SendManyRecipient(address, due, hexMemo);
                zOutputsDeque.push_front(r);

                // reduce the amount being sent right now to the value of all inputs
                value = jsInputValue;
            }
        }

        // create output for recipient
        if (address.empty()) {
            assert(value==0);
            info.vjsout.push_back(JSOutput());  // dummy output while we accumulate funds into a change note for vpub_new
        } else {
            PaymentAddress pa = CZCPaymentAddress(address).Get();
            JSOutput jso = JSOutput(pa, value);
            if (hexMemo.size() > 0) {
                jso.memo = get_memo_from_hex_string(hexMemo);
            }
            info.vjsout.push_back(jso);
        }

        // create output for any change
        if (jsChange>0) {
            info.vjsout.push_back(JSOutput(frompaymentaddress_, jsChange));

            LogPrint("zrpcunsafe", "%s: generating note for change (amount=%s)\n",
                    getId(),
                    FormatMoney(jsChange)
                    );
        }

        obj = perform_joinsplit(info, witnesses, jsAnchor);

        if (jsChange > 0) {
            changeOutputIndex = find_output(obj, 1);
        }
    }

    // Sanity check in case changes to code block above exits loop by invoking 'break'
    assert(zInputsDeque.size() == 0);
    assert(zOutputsDeque.size() == 0);
    assert(vpubNewProcessed);

    sign_send_raw_transaction(obj);
    return true;
}


/**
 * Sign and send a raw transaction.
 * Raw transaction as hex string should be in object field "rawtxn"
 */
void AsyncRPCOperation_sendmany::sign_send_raw_transaction(UniValue obj)
{
    // Sign the raw transaction
    UniValue rawtxnValue = find_value(obj, "rawtxn");
    if (rawtxnValue.isNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Missing hex data for raw transaction");
    }
    std::string rawtxn = rawtxnValue.get_str();

    UniValue params = UniValue(UniValue::VARR);
    params.push_back(rawtxn);
    UniValue signResultValue = signrawtransaction(params, false);
    UniValue signResultObject = signResultValue.get_obj();
    UniValue completeValue = find_value(signResultObject, "complete");
    bool complete = completeValue.get_bool();
    if (!complete) {
        // TODO: #1366 Maybe get "errors" and print array vErrors into a string
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Failed to sign transaction");
    }

    UniValue hexValue = find_value(signResultObject, "hex");
    if (hexValue.isNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Missing hex data for signed transaction");
    }
    std::string signedtxn = hexValue.get_str();

    // Send the signed transaction
    if (!testmode) {
        params.clear();
        params.setArray();
        params.push_back(signedtxn);
        UniValue sendResultValue = sendrawtransaction(params, false);
        if (sendResultValue.isNull()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Send raw transaction did not return an error or a txid.");
        }

        std::string txid = sendResultValue.get_str();

        UniValue o(UniValue::VOBJ);
        o.pushKV("txid", txid);
        set_result(o);
    } else {
        // Test mode does not send the transaction to the network.

        CDataStream stream(ParseHex(signedtxn), SER_NETWORK, PROTOCOL_VERSION);
        CTransaction tx;
        stream >> tx;

        UniValue o(UniValue::VOBJ);
        o.pushKV("test", 1);
        o.pushKV("txid", tx.GetHash().ToString());
        o.pushKV("hex", signedtxn);
        set_result(o);
    }

    // Keep the signed transaction so we can hash to the same txid
    CDataStream stream(ParseHex(signedtxn), SER_NETWORK, PROTOCOL_VERSION);
    CTransaction tx;
    stream >> tx;
    tx_ = tx;
}


bool AsyncRPCOperation_sendmany::find_utxos(bool fAcceptCoinbase=false) {
    set<CBitcoinAddress> setAddress = {fromtaddr_};
    vector<COutput> vecOutputs;

    LOCK2(cs_main, pwalletMain->cs_wallet);

    pwalletMain->AvailableCoins(vecOutputs, false, NULL, true, fAcceptCoinbase, fAcceptCoinbase);

    BOOST_FOREACH(const COutput& out, vecOutputs) {
        if (!out.fSpendable) {
            continue;
        }

        if (out.nDepth < mindepth_) {
            continue;
        }

        if (setAddress.size()) {
            CTxDestination address;
            if (!ExtractDestination(out.tx->getTxBase()->GetVout()[out.pos].scriptPubKey, address)) {
                continue;
            }

            if (!setAddress.count(address)) {
                continue;
            }
        }

        // By default we ignore coinbase outputs
        bool isCoinbase = out.tx->getTxBase()->IsCoinBase();
        if (isCoinbase && fAcceptCoinbase==false) {
            continue;
        }

        CAmount nValue = out.tx->getTxBase()->GetVout()[out.pos].nValue;
        SendManyInputUTXO utxo(out.tx->getTxBase()->GetHash(), out.pos, nValue, isCoinbase);
        t_inputs_.push_back(utxo);
    }

    // sort in ascending order, so smaller utxos appear first
    std::sort(t_inputs_.begin(), t_inputs_.end(), [](SendManyInputUTXO i, SendManyInputUTXO j) -> bool {
        return ( std::get<2>(i) < std::get<2>(j));
    });

    return t_inputs_.size() > 0;
}


bool AsyncRPCOperation_sendmany::find_unspent_notes() {
    std::vector<CNotePlaintextEntry> entries;
    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        pwalletMain->GetFilteredNotes(entries, fromaddress_, mindepth_);
    }

    for (CNotePlaintextEntry & entry : entries) {
        z_inputs_.push_back(SendManyInputJSOP(entry.jsop, entry.plaintext.note(boost::get<libzcash::PaymentAddress>(frompaymentaddress_)), CAmount(entry.plaintext.value())));
        std::string data(entry.plaintext.memo().begin(), entry.plaintext.memo().end());
        LogPrint("zrpcunsafe", "%s: found unspent note (txid=%s, vjoinsplit=%d, ciphertext=%d, amount=%s, memo=%s)\n",
            getId(),
            entry.jsop.hash.ToString().substr(0, 10),
            entry.jsop.js,
            int(entry.jsop.n),  // uint8_t
            FormatMoney(entry.plaintext.value()),
            HexStr(data).substr(0, 10)
            );
    }

    if (z_inputs_.size() == 0) {
        return false;
    }

    // sort in descending order, so big notes appear first
    std::sort(z_inputs_.begin(), z_inputs_.end(), [](SendManyInputJSOP i, SendManyInputJSOP j) -> bool {
        return ( std::get<2>(i) > std::get<2>(j));
    });

    return true;
}

UniValue AsyncRPCOperation_sendmany::perform_joinsplit(AsyncJoinSplitInfo & info) {
    std::vector<std::optional < ZCIncrementalWitness>> witnesses;
    uint256 anchor;
    {
        LOCK(cs_main);
        anchor = pcoinsTip->GetBestAnchor();    // As there are no inputs, ask the wallet for the best anchor
    }
    return perform_joinsplit(info, witnesses, anchor);
}


UniValue AsyncRPCOperation_sendmany::perform_joinsplit(AsyncJoinSplitInfo & info, std::vector<JSOutPoint> & outPoints) {
    std::vector<std::optional < ZCIncrementalWitness>> witnesses;
    uint256 anchor;
    {
        LOCK(cs_main);
        pwalletMain->GetNoteWitnesses(outPoints, witnesses, anchor);
    }
    return perform_joinsplit(info, witnesses, anchor);
}

UniValue AsyncRPCOperation_sendmany::perform_joinsplit(
        AsyncJoinSplitInfo & info,
        std::vector<std::optional < ZCIncrementalWitness>> witnesses,
        uint256 anchor)
{
    if (anchor.IsNull()) {
        throw std::runtime_error("anchor is null");
    }

    if (!(witnesses.size() == info.notes.size())) {
        throw runtime_error("number of notes and witnesses do not match");
    }

    for (size_t i = 0; i < witnesses.size(); i++) {
        if (!witnesses[i]) {
            throw runtime_error("joinsplit input could not be found in tree");
        }
        info.vjsin.push_back(JSInput(*witnesses[i], info.notes[i], spendingkey_));
    }

    // Make sure there are two inputs and two outputs
    while (info.vjsin.size() < ZC_NUM_JS_INPUTS) {
        info.vjsin.push_back(JSInput());
    }

    while (info.vjsout.size() < ZC_NUM_JS_OUTPUTS) {
        info.vjsout.push_back(JSOutput());
    }

    if (info.vjsout.size() != ZC_NUM_JS_INPUTS || info.vjsin.size() != ZC_NUM_JS_OUTPUTS) {
        throw runtime_error("unsupported joinsplit input/output counts");
    }

    CMutableTransaction mtx(tx_);

    LogPrint("zrpcunsafe", "%s: creating joinsplit at index %d (vpub_old=%s, vpub_new=%s, in[0]=%s, in[1]=%s, out[0]=%s, out[1]=%s)\n",
            getId(),
            tx_.GetVjoinsplit().size(),
            FormatMoney(info.vpub_old), FormatMoney(info.vpub_new),
            FormatMoney(info.vjsin[0].note.value()), FormatMoney(info.vjsin[1].note.value()),
            FormatMoney(info.vjsout[0].value), FormatMoney(info.vjsout[1].value)
            );

    // Generate the proof, this can take over a minute.
    std::array<libzcash::JSInput, ZC_NUM_JS_INPUTS> inputs
            {info.vjsin[0], info.vjsin[1]};
    std::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS> outputs
            {info.vjsout[0], info.vjsout[1]};
        #ifdef __APPLE__
        std::array<uint64_t, ZC_NUM_JS_INPUTS> inputMap;
        std::array<uint64_t, ZC_NUM_JS_OUTPUTS> outputMap;
        #else
        std::array<size_t, ZC_NUM_JS_INPUTS> inputMap;
        std::array<size_t, ZC_NUM_JS_OUTPUTS> outputMap;
        #endif

    uint256 esk; // payment disclosure - secret

    JSDescription jsdesc = JSDescription::Randomized(
            mtx.nVersion == GROTH_TX_VERSION,
            *pzcashParams,
            joinSplitPubKey_,
            anchor,
            inputs,
            outputs,
            inputMap,
            outputMap,
            info.vpub_old,
            info.vpub_new,
            !this->testmode,
            &esk); // parameter expects pointer to esk, so pass in address
    {
        auto verifier = libzcash::ProofVerifier::Strict();
        if (!(jsdesc.Verify(*pzcashParams, verifier, joinSplitPubKey_))) {
            throw std::runtime_error("error verifying joinsplit");
        }
    }

    mtx.vjoinsplit.push_back(jsdesc);

    // Empty output script.
    CScript scriptCode;
    CTransaction signTx(mtx);
    uint256 dataToBeSigned = SignatureHash(scriptCode, signTx, NOT_AN_INPUT, SIGHASH_ALL);

    // Add the signature
    if (!(crypto_sign_detached(&mtx.joinSplitSig[0], NULL,
            dataToBeSigned.begin(), 32,
            joinSplitPrivKey_
            ) == 0))
    {
        throw std::runtime_error("crypto_sign_detached failed");
    }

    // Sanity check
    if (!(crypto_sign_verify_detached(&mtx.joinSplitSig[0],
            dataToBeSigned.begin(), 32,
            mtx.joinSplitPubKey.begin()
            ) == 0))
    {
        throw std::runtime_error("crypto_sign_verify_detached failed");
    }

    CTransaction rawTx(mtx);
    tx_ = rawTx;

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << rawTx;

    std::string encryptedNote1;
    std::string encryptedNote2;
    {
        CDataStream ss2(SER_NETWORK, PROTOCOL_VERSION);
        ss2 << ((unsigned char) 0x00);
        ss2 << jsdesc.ephemeralKey;
        ss2 << jsdesc.ciphertexts[0];
        ss2 << jsdesc.h_sig(*pzcashParams, joinSplitPubKey_);

        encryptedNote1 = HexStr(ss2.begin(), ss2.end());
    }
    {
        CDataStream ss2(SER_NETWORK, PROTOCOL_VERSION);
        ss2 << ((unsigned char) 0x01);
        ss2 << jsdesc.ephemeralKey;
        ss2 << jsdesc.ciphertexts[1];
        ss2 << jsdesc.h_sig(*pzcashParams, joinSplitPubKey_);

        encryptedNote2 = HexStr(ss2.begin(), ss2.end());
    }

    UniValue arrInputMap(UniValue::VARR);
    UniValue arrOutputMap(UniValue::VARR);
    for (size_t i = 0; i < ZC_NUM_JS_INPUTS; i++) {
        arrInputMap.push_back(inputMap[i]);
    }
    for (size_t i = 0; i < ZC_NUM_JS_OUTPUTS; i++) {
        arrOutputMap.push_back(outputMap[i]);
    }


    // !!! Payment disclosure START
    unsigned char buffer[32] = {0};
    memcpy(&buffer[0], &joinSplitPrivKey_[0], 32); // private key in first half of 64 byte buffer
    std::vector<unsigned char> vch(&buffer[0], &buffer[0] + 32);
    uint256 joinSplitPrivKey = uint256(vch);
    size_t js_index = tx_.GetVjoinsplit().size() - 1;
    uint256 placeholder;
    for (int i = 0; i < ZC_NUM_JS_OUTPUTS; i++) {
        uint8_t mapped_index = outputMap[i];
        // placeholder for txid will be filled in later when tx has been finalized and signed.
        PaymentDisclosureKey pdKey = {placeholder, js_index, mapped_index};
        JSOutput output = outputs[mapped_index];
        libzcash::PaymentAddress zaddr = output.addr;  // randomized output
        PaymentDisclosureInfo pdInfo = {PAYMENT_DISCLOSURE_VERSION_EXPERIMENTAL, esk, joinSplitPrivKey, zaddr};
        paymentDisclosureData_.push_back(PaymentDisclosureKeyInfo(pdKey, pdInfo));

        CZCPaymentAddress address(zaddr);
        LogPrint("paymentdisclosure", "%s: Payment Disclosure: js=%d, n=%d, zaddr=%s\n", getId(), js_index, int(mapped_index), address.ToString());
    }
    // !!! Payment disclosure END

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("encryptednote1", encryptedNote1);
    obj.pushKV("encryptednote2", encryptedNote2);
    obj.pushKV("rawtxn", HexStr(ss.begin(), ss.end()));
    obj.pushKV("inputmap", arrInputMap);
    obj.pushKV("outputmap", arrOutputMap);
    return obj;
}

void AsyncRPCOperation_sendmany::add_taddr_outputs_to_tx() {

    CMutableTransaction rawTx(tx_);

    for (SendManyRecipient & r : t_outputs_) {
        std::string outputAddress = std::get<0>(r);
        CAmount nAmount = std::get<1>(r);

        CBitcoinAddress address(outputAddress);
        if (!address.IsValid()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid output address, not a valid taddr.");
        }

        CScript scriptPubKey = GetScriptForDestination(address.Get());
        rawTx.addOut(CTxOut(nAmount, scriptPubKey));
    }

    tx_ = CTransaction(rawTx);
}

void AsyncRPCOperation_sendmany::add_taddr_change_output_to_tx(CAmount amount, bool sendChangeToSource) {

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();
    CTxOut out;
    if (sendChangeToSource) {
        CScript scriptPubKey = GetScriptForDestination(fromtaddr_.Get());
        out = CTxOut(amount, scriptPubKey);
    }
    else {
        CReserveKey keyChange(pwalletMain);
        CPubKey vchPubKey;
        bool ret = keyChange.GetReservedKey(vchPubKey);
        if (!ret) {
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Could not generate a taddr to use as a change address"); // should never fail, as we just unlocked
        }
        CScript scriptPubKey = GetScriptForDestination(vchPubKey.GetID());
        out =  CTxOut(amount, scriptPubKey);
    }

    CMutableTransaction rawTx(tx_);
    rawTx.addOut(out);
    tx_ = CTransaction(rawTx);
}

std::array<unsigned char, ZC_MEMO_SIZE> AsyncRPCOperation_sendmany::get_memo_from_hex_string(const std::string& s) {
    std::array<unsigned char, ZC_MEMO_SIZE> memo = {{0x00}};

    std::vector<unsigned char> rawMemo = ParseHex(s.c_str());

    // If ParseHex comes across a non-hex char, it will stop but still return results so far.
    size_t slen = s.length();
    if (slen % 2 !=0 || (slen>0 && rawMemo.size()!=slen/2)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Memo must be in hexadecimal format");
    }

    if (rawMemo.size() > ZC_MEMO_SIZE) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Memo size of %d is too big, maximum allowed is %d", rawMemo.size(), ZC_MEMO_SIZE));
    }

    // copy vector into boost array
    int lenMemo = rawMemo.size();
    for (int i = 0; i < ZC_MEMO_SIZE && i < lenMemo; i++) {
        memo[i] = rawMemo[i];
    }
    return memo;
}

/**
 * Override getStatus() to append the operation's input parameters to the default status object.
 */
UniValue AsyncRPCOperation_sendmany::getStatus() const {
    UniValue v = AsyncRPCOperation::getStatus();
    if (contextinfo_.isNull()) {
        return v;
    }

    UniValue obj = v.get_obj();
    obj.pushKV("method", "z_sendmany");
    obj.pushKV("params", contextinfo_ );
    return obj;
}

