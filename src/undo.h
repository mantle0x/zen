// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2013 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UNDO_H
#define BITCOIN_UNDO_H

#include "compressor.h" 
#include "primitives/transaction.h"
#include "serialize.h"
#include "coins.h"
#include "sc/proofverifier.h"

/** Undo information for a CTxIn
 *
 *  Contains the prevout's CTxOut being spent, and if this was the
 *  last output of the affected transaction, its metadata as well
 *  (coinbase or not, height, transaction version, originScid)
 *  Following the introduction of sidechain certificates and backward transfer,
 *  nFirstBwtPos is serialized for certificates.
 */
class CTxInUndo
{
public:
    CTxOut txout;           // the txout data before being spent
    bool fCoinBase;         // if the outpoint was the last unspent: whether it belonged to a coinbase
    unsigned int nHeight;   // if the outpoint was the last unspent: its height
    int nVersion;           // if the outpoint was the last unspent: its version
    int nFirstBwtPos;       // if the outpoint was the last unspent: its nFirstBwtPos, serialized only for certificates
    int nBwtMaturityHeight; // if the outpoint was the last unspent: its nBwtMaturityHeight, introduced with certificates

    CTxInUndo() : txout(), fCoinBase(false), nHeight(0), nVersion(0), nFirstBwtPos(BWT_POS_UNSET), nBwtMaturityHeight(0) {}
    CTxInUndo(const CTxOut &txoutIn, bool fCoinBaseIn = false,
              unsigned int nHeightIn = 0, int nVersionIn = 0,
              int firstBwtPos = BWT_POS_UNSET, int bwtMaturityHeight = 0):
        txout(txoutIn), fCoinBase(fCoinBaseIn), nHeight(nHeightIn), nVersion(nVersionIn),
        nFirstBwtPos(firstBwtPos), nBwtMaturityHeight(bwtMaturityHeight) {}

    unsigned int GetSerializeSize(int nType, int nVersion) const {
        unsigned int totalSize = 0;
        totalSize = ::GetSerializeSize(VARINT(nHeight*2+(fCoinBase ? 1 : 0)), nType, nVersion) +
               (nHeight > 0 ? ::GetSerializeSize(VARINT(this->nVersion), nType, nVersion) : 0) +
               ::GetSerializeSize(CTxOutCompressor(REF(txout)), nType, nVersion);
        if ((nHeight > 0) && ((this->nVersion & 0x7f) == (SC_CERT_VERSION & 0x7f))) {
            totalSize += ::GetSerializeSize(nFirstBwtPos, nType,nVersion);
            totalSize += ::GetSerializeSize(nBwtMaturityHeight, nType,nVersion);
        }
        return totalSize;
    }

    template<typename Stream>
    void Serialize(Stream &s, int nType, int nVersion) const {
        ::Serialize(s, VARINT(nHeight*2+(fCoinBase ? 1 : 0)), nType, nVersion);
        if (nHeight > 0)
            ::Serialize(s, VARINT(this->nVersion), nType, nVersion);
        ::Serialize(s, CTxOutCompressor(REF(txout)), nType, nVersion);

        if ((nHeight > 0) && ((this->nVersion & 0x7f) == (SC_CERT_VERSION & 0x7f))) {
            ::Serialize(s, nFirstBwtPos, nType, nVersion);
            ::Serialize(s, nBwtMaturityHeight, nType, nVersion);
        }
    }

    template<typename Stream>
    void Unserialize(Stream &s, int nType, int nVersion) {
        unsigned int nCode = 0;
        ::Unserialize(s, VARINT(nCode), nType, nVersion);
        nHeight = nCode / 2;
        fCoinBase = nCode & 1;
        if (nHeight > 0)
            ::Unserialize(s, VARINT(this->nVersion), nType, nVersion);
        ::Unserialize(s, REF(CTxOutCompressor(REF(txout))), nType, nVersion);

        if ((nHeight > 0) && ((this->nVersion & 0x7f) == (SC_CERT_VERSION & 0x7f))) {
            ::Unserialize(s, nFirstBwtPos, nType, nVersion);
            ::Unserialize(s, nBwtMaturityHeight, nType, nVersion);
        }
    }

    std::string ToString() const
    {
        std::string str;
        str += strprintf("txout(%s)\n", txout.ToString());
        str += strprintf("        fCoinBase         = %d\n", fCoinBase);
        str += strprintf("        nHeight           = %d\n", nHeight);
        str += strprintf("        nVersion          = %x\n", nVersion);
        str += strprintf("        nFirstBwtPos      = %d\n", nFirstBwtPos);
        str += strprintf("        nBwtMaturityHeight= %d\n", nBwtMaturityHeight);
        return str;
    }

};

/** Undo information for a CTransaction */
class CTxUndo
{
public:
    std::vector<CTxInUndo> vprevout; // undo information for all txins
    CTxUndo(): vprevout() {}

    size_t GetSerializeSize(int nType, int nVersion) const
    {
        CSizeComputer s(nType, nVersion);
        NCONST_PTR(this)->Serialize(s, nType, nVersion);
        return s.size();
    };

    template<typename Stream>
    void Serialize(Stream& s, int nType, int nVersion) const
    {
        ::Serialize(s, vprevout, nType, nVersion);
    };

    template<typename Stream>
    void Unserialize(Stream& s, int nType, int nVersion)
    {
        ::Unserialize(s, vprevout, nType, nVersion);
    };

    std::string ToString() const
    {
        std::string str;
        str += strprintf("vprevout.size %u\n", vprevout.size());
        for(const CTxInUndo& in: vprevout)
            str += strprintf("\n  [%s]\n", in.ToString());

        return str;
    }
};

struct CSidechainUndoData
{
    uint32_t sidechainUndoDataVersion;
    enum AvailableSections : uint8_t
    {
        UNDEFINED               = 0,
        SIDECHAIN_STATE         = 1,
        MATURED_AMOUNTS         = 2,
        SUPERSEDED_CERT_DATA    = 4,
        CEASED_CERTIFICATE_DATA = 8
    };
    uint8_t contentBitMask;

    // SIDECHAIN_STATE section
    int prevTopCommittedCertReferencedEpoch;
    uint256 prevTopCommittedCertHash;
    int64_t prevTopCommittedCertQuality;
    CAmount prevTopCommittedCertBwtAmount;
    libzendoomc::ScFieldElement prevTopCommittedCertDataHash;

    // MATURED_AMOUNTS section
    CAmount appliedMaturedAmount;

    // LOW_QUALITY_CERT_DATA
    std::vector<CTxInUndo> lowQualityBwts;

    // CEASED_CERTIFICATE_DATA
    std::vector<CTxInUndo> ceasedBwts;

    CSidechainUndoData(): sidechainUndoDataVersion(0), contentBitMask(AvailableSections::UNDEFINED),
        prevTopCommittedCertReferencedEpoch(CScCertificate::EPOCH_NULL), prevTopCommittedCertHash(),
        prevTopCommittedCertQuality(CScCertificate::QUALITY_NULL), prevTopCommittedCertBwtAmount(0),
        prevTopCommittedCertDataHash(), appliedMaturedAmount(0), lowQualityBwts(), ceasedBwts() {}

    size_t GetSerializeSize(int nType, int nVersion) const
    {
        unsigned int totalSize = ::GetSerializeSize(sidechainUndoDataVersion, nType, nVersion);
        totalSize += ::GetSerializeSize(contentBitMask, nType, nVersion);
        if (contentBitMask & AvailableSections::SIDECHAIN_STATE)
        {
            totalSize += ::GetSerializeSize(prevTopCommittedCertReferencedEpoch, nType, nVersion);
            totalSize += ::GetSerializeSize(prevTopCommittedCertHash,            nType, nVersion);
            totalSize += ::GetSerializeSize(prevTopCommittedCertQuality,         nType, nVersion);
            totalSize += ::GetSerializeSize(prevTopCommittedCertBwtAmount,       nType, nVersion);
            totalSize += ::GetSerializeSize(prevTopCommittedCertDataHash,        nType, nVersion);
        }
        if (contentBitMask & AvailableSections::MATURED_AMOUNTS)
        {
            totalSize += ::GetSerializeSize(appliedMaturedAmount, nType, nVersion);
        }
        if (contentBitMask & AvailableSections::SUPERSEDED_CERT_DATA)
        {
            totalSize += ::GetSerializeSize(lowQualityBwts, nType, nVersion);
        }
        if (contentBitMask & AvailableSections::CEASED_CERTIFICATE_DATA)
        {
            totalSize += ::GetSerializeSize(ceasedBwts, nType, nVersion);
        }
        return totalSize;
    }

    template<typename Stream>
    void Serialize(Stream& s, int nType, int nVersion) const
    {
        ::Serialize(s, sidechainUndoDataVersion, nType, nVersion);
        ::Serialize(s, contentBitMask, nType, nVersion);
        if (contentBitMask & AvailableSections::SIDECHAIN_STATE)
        {
            ::Serialize(s, prevTopCommittedCertReferencedEpoch, nType, nVersion);
            ::Serialize(s, prevTopCommittedCertHash,            nType, nVersion);
            ::Serialize(s, prevTopCommittedCertQuality,         nType, nVersion);
            ::Serialize(s, prevTopCommittedCertBwtAmount,       nType, nVersion);
            ::Serialize(s, prevTopCommittedCertDataHash,        nType, nVersion);
        }
        if (contentBitMask & AvailableSections::MATURED_AMOUNTS)
        {
            ::Serialize(s, appliedMaturedAmount, nType, nVersion);
        }
        if (contentBitMask & AvailableSections::SUPERSEDED_CERT_DATA)
        {
            ::Serialize(s, lowQualityBwts, nType, nVersion);
        }
        if (contentBitMask & AvailableSections::CEASED_CERTIFICATE_DATA)
        {
            ::Serialize(s, ceasedBwts, nType, nVersion);
        }
        return;
    }

    template<typename Stream>
    void Unserialize(Stream& s, int nType, int nVersion)
    {
        ::Unserialize(s, sidechainUndoDataVersion, nType, nVersion);
        ::Unserialize(s, contentBitMask, nType, nVersion);
        if (contentBitMask & AvailableSections::SIDECHAIN_STATE)
        {
            ::Unserialize(s, prevTopCommittedCertReferencedEpoch, nType, nVersion);
            ::Unserialize(s, prevTopCommittedCertHash,            nType, nVersion);
            ::Unserialize(s, prevTopCommittedCertQuality,         nType, nVersion);
            ::Unserialize(s, prevTopCommittedCertBwtAmount,       nType, nVersion);
            ::Unserialize(s, prevTopCommittedCertDataHash,        nType, nVersion);
        } 
        if (contentBitMask & AvailableSections::MATURED_AMOUNTS)
        {
            ::Unserialize(s, appliedMaturedAmount, nType, nVersion);
        }
        if (contentBitMask & AvailableSections::SUPERSEDED_CERT_DATA)
        {
            ::Unserialize(s, lowQualityBwts, nType, nVersion);
        }
        if (contentBitMask & AvailableSections::CEASED_CERTIFICATE_DATA)
        {
            ::Unserialize(s, ceasedBwts, nType, nVersion);
        }
        return;
    }

    std::string ToString() const
    {
        std::string res;
        res += strprintf("contentBitMask=%u\n", contentBitMask);
        if (contentBitMask & AvailableSections::SIDECHAIN_STATE)
        {
            res += strprintf("prevTopCommittedCertReferencedEpoch=%d\n", prevTopCommittedCertReferencedEpoch);
            res += strprintf("prevTopCommittedCertHash=%s\n", prevTopCommittedCertHash.ToString());
            res += strprintf("prevTopCommittedCertQuality=%d\n", prevTopCommittedCertQuality);
            res += strprintf("prevTopCommittedCertBwtAmount=%d.%08d\n", prevTopCommittedCertBwtAmount / COIN, prevTopCommittedCertBwtAmount % COIN);
            res += strprintf("prevTopCommittedCertDataHash=%s\n", prevTopCommittedCertDataHash.ToString());
        }

        if (contentBitMask & AvailableSections::MATURED_AMOUNTS)
            res += strprintf("appliedMaturedAmount=%d.%08d\n", appliedMaturedAmount / COIN, appliedMaturedAmount % COIN);

        res += strprintf("ceasedBwts.size()=%u\n", ceasedBwts.size());
        for(const auto& voidCertOutput: ceasedBwts)
           res += voidCertOutput.ToString() + "\n";

        res += strprintf("lowQualityBwts.size %u\n", lowQualityBwts.size());
        for(const auto& voidCertUndo: lowQualityBwts)
            res += voidCertUndo.ToString() + "\n";

        return res;
    }
};

/** Undo information for a CBlock */
class CBlockUndo
{
    /** Magic number read from the value expressing the size of vtxundo vector.
     *  It is used for distinguish new version of CBlockUndo instance from old ones.
     *  The maximum number of tx in a block is roughly MAX_BLOCK_SIZE / MIN_TX_SIZE, which is:
     *   2M / 61bytes =~ 33K = 0x8012
     * Therefore the magic number must be a number greater than this limit. */
    static const uint16_t _marker = 0xffff;

    static_assert(_marker > (MAX_BLOCK_SIZE / MIN_TX_SIZE),
        "CBlockUndo::_marker must be greater than max number of tx in a block!");

    /** memory only */
    bool includesSidechainAttributes;

public:
    std::vector<CTxUndo> vtxundo;
    uint256 old_tree_root;
    std::map<uint256, CSidechainUndoData> scUndoDatabyScId;

    /** create as new */
    CBlockUndo() : includesSidechainAttributes(true) {}

    size_t GetSerializeSize(int nType, int nVersion) const
    {
        CSizeComputer s(nType, nVersion);
        NCONST_PTR(this)->Serialize(s, nType, nVersion);
        return s.size();
    }

    template<typename Stream>
    void Serialize(Stream& s, int nType, int nVersion) const
    {
        if (includesSidechainAttributes)
        {
            WriteCompactSize(s, _marker);
            ::Serialize(s, vtxundo, nType, nVersion);
            ::Serialize(s, old_tree_root, nType, nVersion);
            ::Serialize(s, scUndoDatabyScId, nType, nVersion);
        }
        else
        {
            ::Serialize(s, (vtxundo), nType, nVersion);
            ::Serialize(s, (old_tree_root), nType, nVersion);
        }
    }

    template<typename Stream>
    void Unserialize(Stream& s, int nType, int nVersion)
    {
        // reading from data stream to memory
        vtxundo.clear();
        includesSidechainAttributes = false;

        unsigned int nSize = ReadCompactSize(s);
        if (nSize == _marker)
        {
            // this is a new version of blockundo
            ::Unserialize(s, (vtxundo), nType, nVersion);
            ::Unserialize(s, (old_tree_root), nType, nVersion);
            ::Unserialize(s, scUndoDatabyScId, nType, nVersion);
            includesSidechainAttributes = true;
        }
        else
        {
            // vtxundo size has been already consumed in stream, add its entries
            ::AddEntriesInVector(s, vtxundo, nType, nVersion, nSize);
            ::Unserialize(s, (old_tree_root), nType, nVersion);
        }
    };

    std::string ToString() const
    {
        std::string str = "\n=== CBlockUndo START ===========================================================================\n";
        str += strprintf("includesSidechainAttributes=%u (mem only)\n", includesSidechainAttributes);
        str += strprintf("vtxundo.size %u\n", vtxundo.size());

        for(const CTxUndo& txUndo: vtxundo)
            str += txUndo.ToString() + "\n";

        str += strprintf("old_tree_root %s\n", old_tree_root.ToString().substr(0,10));

        for (auto entry : scUndoDatabyScId)
            str += strprintf("%s --> %s\n", entry.first.ToString().substr(0,10), entry.second.ToString());

        str += strprintf(" ---> obj size %u\n", GetSerializeSize(SER_NETWORK, PROTOCOL_VERSION));
        CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
        hasher << *this;
        str += strprintf("      obj hash [%s]\n", hasher.GetHash().ToString());
        str += "=== CBlockUndo END =============================================================================\n";
        return str;
    }

    /** for testing */
    bool IncludesSidechainAttributes() const  { return includesSidechainAttributes; }

};
#endif // BITCOIN_UNDO_H
