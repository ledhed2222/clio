#include <ripple/app/tx/impl/details/NFTokenUtils.h>
#include <ripple/protocol/STBase.h>
#include <ripple/protocol/STTx.h>
#include <ripple/protocol/TxMeta.h>
#include <vector>

#include <backend/BackendInterface.h>
#include <backend/DBHelpers.h>
#include <backend/Types.h>

static std::
    pair<std::vector<NFTokenTransactionsData>, std::optional<NFTokensData>>
    getNFTokenMintData(ripple::TxMeta const& txMeta, ripple::STTx const& sttx)
{
    // We cannot determine anything we would want to store from an
    // unsuccessful mint
    if (txMeta.getResultTER() != ripple::tesSUCCESS)
        return {{}, {}};

    // To find the minted token ID, we put all tokenIDs referenced in the
    // metadata from prior to the tx application into one vector, then all
    // tokenIDs refrenced in the metadata from after the tx application into
    // another, then find the one tokenID that was added by this tx
    // application.
    std::vector<ripple::uint256> prevIDs;
    std::vector<ripple::uint256> finalIDs;

    for (ripple::STObject const& node : txMeta.getNodes())
    {
        if (node.getFieldU16(ripple::sfLedgerEntryType) !=
            ripple::ltNFTOKEN_PAGE)
            continue;

        if (node.getFName() == ripple::sfCreatedNode)
        {
            ripple::STArray const& toAddNFTs =
                node.peekAtField(ripple::sfNewFields)
                    .downcast<ripple::STObject>()
                    .getFieldArray(ripple::sfNonFungibleTokens);
            std::transform(
                toAddNFTs.begin(),
                toAddNFTs.end(),
                std::back_inserter(finalIDs),
                [](ripple::STObject const& nft) {
                    return nft.getFieldH256(ripple::sfTokenID);
                });
        }
        else if (node.getFName() == ripple::sfModifiedNode)
        {
            ripple::STArray const& toAddNFTs =
                node.peekAtField(ripple::sfPreviousFields)
                    .downcast<ripple::STObject>()
                    .getFieldArray(ripple::sfNonFungibleTokens);
            std::transform(
                toAddNFTs.begin(),
                toAddNFTs.end(),
                std::back_inserter(prevIDs),
                [](ripple::STObject const& nft) {
                    return nft.getFieldH256(ripple::sfTokenID);
                });

            ripple::STArray const& toAddFinalNFTs =
                node.peekAtField(ripple::sfFinalFields)
                    .downcast<ripple::STObject>()
                    .getFieldArray(ripple::sfNonFungibleTokens);
            std::transform(
                toAddFinalNFTs.begin(),
                toAddFinalNFTs.end(),
                std::back_inserter(finalIDs),
                [](ripple::STObject const& nft) {
                    return nft.getFieldH256(ripple::sfTokenID);
                });
        }
        else
        {
            ripple::STArray const& toAddNFTs =
                node.peekAtField(ripple::sfFinalFields)
                    .downcast<ripple::STObject>()
                    .getFieldArray(ripple::sfNonFungibleTokens);
            std::transform(
                toAddNFTs.begin(),
                toAddNFTs.end(),
                std::back_inserter(prevIDs),
                [](ripple::STObject const& nft) {
                    return nft.getFieldH256(ripple::sfTokenID);
                });
        }
    }

    std::vector<ripple::uint256> tokenIDResult;
    std::set_difference(
        finalIDs.begin(),
        finalIDs.end(),
        prevIDs.begin(),
        prevIDs.end(),
        std::inserter(tokenIDResult, tokenIDResult.begin()));
    if (tokenIDResult.size() == 1)
    {
        return {
            {NFTokenTransactionsData(
                tokenIDResult.front(), txMeta, sttx.getTransactionID())},
            NFTokensData(
                tokenIDResult.front(),
                ripple::nft::getIssuer(tokenIDResult.front()),
                txMeta,
                false)};
    }

    std::stringstream msg;
    msg << __func__ << " - unexpected NFTokenMint data";
    throw std::runtime_error(msg.str());
}

static std::
    pair<std::vector<NFTokenTransactionsData>, std::optional<NFTokensData>>
    getNFTokenBurnData(ripple::TxMeta const& txMeta, ripple::STTx const& sttx)
{
    ripple::uint256 tokenID = sttx.getFieldH256(ripple::sfTokenID);
    std::vector<NFTokenTransactionsData> txs = {
        NFTokenTransactionsData(tokenID, txMeta, sttx.getTransactionID())};

    // If unsuccessful, it did not change the state of the nft so just record
    // the tx itself.
    if (txMeta.getResultTER() != ripple::tesSUCCESS)
        return {txs, {}};

    // Determine who owned the token when it was burned by finding an
    // NFTokenPage that was deleted or modified that contains this
    // tokenID.
    for (ripple::STObject const& node : txMeta.getNodes())
    {
        if (node.getFieldU16(ripple::sfLedgerEntryType) !=
                ripple::ltNFTOKEN_PAGE ||
            node.getFName() == ripple::sfCreatedNode)
            continue;

        // NFT burn can result in an NFTokenPage being modified to no longer
        // include the target, or an NFTokenPage being deleted. If this is
        // modified, we want to look for the target in the fields prior to
        // modification. If deleted, "FinalFields" is overloaded to mean the
        // state prior to deletion.
        ripple::STArray const& prevNFTs = [node]() {
            if (node.getFName() == ripple::sfModifiedNode)
                return node.peekAtField(ripple::sfPreviousFields)
                    .downcast<ripple::STObject>()
                    .getFieldArray(ripple::sfNonFungibleTokens);
            return node.peekAtField(ripple::sfFinalFields)
                .downcast<ripple::STObject>()
                .getFieldArray(ripple::sfNonFungibleTokens);
        }();

        auto nft = std::find_if(
            prevNFTs.begin(),
            prevNFTs.end(),
            [tokenID](ripple::STObject const& candidate) {
                return candidate.getFieldH256(ripple::sfTokenID) == tokenID;
            });
        if (nft != prevNFTs.end())
            return std::make_pair(
                txs,
                NFTokensData(
                    tokenID,
                    ripple::AccountID::fromVoid(
                        node.getFieldH256(ripple::sfLedgerIndex).data()),
                    txMeta,
                    true));
    }

    std::stringstream msg;
    msg << __func__ << " - could not determine owner at burntime";
    throw std::runtime_error(msg.str());
}

static std::
    pair<std::vector<NFTokenTransactionsData>, std::optional<NFTokensData>>
    getNFTokenAcceptOfferData(
        ripple::TxMeta const& txMeta,
        ripple::STTx const& sttx,
        ripple::LedgerIndex seq,
        std::shared_ptr<BackendInterface> backend_)
{
    // If an NFTokenAcceptOffer was unsuccessful, the only way to get the
    // original tokenID is from the offer referenced in the tx. We do this to be
    // able to insert the failed tx into the nf_token_transactions table.
    // However, this means the nf_tokens table will not change so the optional
    // is always empty.
    if (txMeta.getResultTER() != ripple::tesSUCCESS)
    {
        // Even if this is brokered mode with multiple offers, all offers must
        // relate to the same NFT so it doesn't matter which we pick.
        ripple::uint256 offerID = sttx.isFieldPresent(ripple::sfBuyOffer)
            ? sttx.getFieldH256(ripple::sfBuyOffer)
            : sttx.getFieldH256(ripple::sfSellOffer);

        std::optional<Backend::Blob> offer = Backend::synchronous(
            [backend_, offerID, seq](boost::asio::yield_context& yield) {
                return backend_->fetchLedgerObject(offerID, seq - 1, yield);
            });
        if (!offer.has_value())
            return {{}, {}};

        ripple::SLE sle{
            ripple::SerialIter{(*offer).data(), (*offer).size()}, offerID};
        return {
            {NFTokenTransactionsData(
                sle.getFieldH256(ripple::sfTokenID),
                txMeta,
                sttx.getTransactionID())},
            {}};
    }

    // If we have the buy offer from this tx, we can determine the owner
    // more easily by just looking at the owner of the accepted NFTokenOffer
    // object.
    if (sttx.isFieldPresent(ripple::sfBuyOffer))
    {
        auto affectedBuyOffer = std::find_if(
            txMeta.getNodes().begin(),
            txMeta.getNodes().end(),
            [](ripple::STObject const& node) {
                return node.getFieldU16(ripple::sfLedgerEntryType) ==
                    ripple::ltNFTOKEN_OFFER &&
                    !node.isFlag(ripple::lsfSellToken);
            });
        if (affectedBuyOffer == txMeta.getNodes().end())
            throw std::runtime_error("Unexpected NFTokenAcceptOffer data");

        ripple::uint256 tokenID = (*affectedBuyOffer)
                                      .peekAtField(ripple::sfFinalFields)
                                      .downcast<ripple::STObject>()
                                      .getFieldH256(ripple::sfTokenID);

        ripple::AccountID owner = (*affectedBuyOffer)
                                      .peekAtField(ripple::sfFinalFields)
                                      .downcast<ripple::STObject>()
                                      .getAccountID(ripple::sfOwner);
        return {
            {NFTokenTransactionsData(tokenID, txMeta, sttx.getTransactionID())},
            NFTokensData(tokenID, owner, txMeta, false)};
    }

    // Otherwise we have to infer the new owner from the affected nodes.
    auto affectedSellOffer = std::find_if(
        txMeta.getNodes().begin(),
        txMeta.getNodes().end(),
        [](ripple::STObject const& node) {
            return node.getFieldU16(ripple::sfLedgerEntryType) ==
                ripple::ltNFTOKEN_OFFER &&
                node.isFlag(ripple::lsfSellToken);
        });
    if (affectedSellOffer == txMeta.getNodes().end())
        throw std::runtime_error("Unexpected NFTokenAcceptOffer data");

    ripple::uint256 tokenID = (*affectedSellOffer)
                                  .peekAtField(ripple::sfFinalFields)
                                  .downcast<ripple::STObject>()
                                  .getFieldH256(ripple::sfTokenID);

    ripple::AccountID seller = (*affectedSellOffer)
                                   .peekAtField(ripple::sfFinalFields)
                                   .downcast<ripple::STObject>()
                                   .getAccountID(ripple::sfOwner);

    for (ripple::STObject const& node : txMeta.getNodes())
    {
        if (node.getFieldU16(ripple::sfLedgerEntryType) !=
                ripple::ltNFTOKEN_PAGE ||
            node.getFName() == ripple::sfDeletedNode)
            continue;

        ripple::AccountID nodeOwner = ripple::AccountID::fromVoid(
            node.getFieldH256(ripple::sfLedgerIndex).data());
        if (nodeOwner == seller)
            continue;

        ripple::STArray const& nfts = [node]() {
            if (node.getFName() == ripple::sfCreatedNode)
                return node.peekAtField(ripple::sfNewFields)
                    .downcast<ripple::STObject>()
                    .getFieldArray(ripple::sfNonFungibleTokens);
            return node.peekAtField(ripple::sfFinalFields)
                .downcast<ripple::STObject>()
                .getFieldArray(ripple::sfNonFungibleTokens);
        }();

        auto nft = std::find_if(
            nfts.begin(),
            nfts.end(),
            [tokenID](ripple::STObject const& candidate) {
                return candidate.getFieldH256(ripple::sfTokenID) == tokenID;
            });
        if (nft != nfts.end())
            return {
                {NFTokenTransactionsData(
                    tokenID, txMeta, sttx.getTransactionID())},
                NFTokensData(tokenID, nodeOwner, txMeta, false)};
    }

    std::stringstream msg;
    msg << __func__ << " - unexpected NFTokenAcceptOffer tx data";
    throw std::runtime_error(msg.str());
}

// This is the only transaction where there can be more than 1 element in
// the returned vector, because you can cancel multiple offers in one
// transaction using this feature. This transaction also never returns an
// NFTokensData because it does not change the state of an NFT itself.
static std::
    pair<std::vector<NFTokenTransactionsData>, std::optional<NFTokensData>>
    getNFTokenCancelOfferData(
        ripple::TxMeta const& txMeta,
        ripple::STTx const& sttx,
        ripple::LedgerIndex seq,
        std::shared_ptr<BackendInterface> backend_)
{
    std::vector<NFTokenTransactionsData> txs;

    // If this was unsuccessful, the only way to get the tokenID is to find the
    // original offers.
    if (txMeta.getResultTER() != ripple::tesSUCCESS)
    {
        std::vector<ripple::uint256> const& offerIDs =
            sttx.getFieldV256(ripple::sfTokenOffers).value();

        auto offers = Backend::synchronous(
            [backend_, offerIDs, seq](boost::asio::yield_context& yield) {
                return backend_->fetchLedgerObjects(offerIDs, seq - 1, yield);
            });
        for (size_t i = 0; i < offerIDs.size(); i++)
        {
            if (!offers[i].size())
                continue;

            auto&& offer = offers[i];
            ripple::SLE sle{
                ripple::SerialIter{offer.data(), offer.size()}, offerIDs[i]};
            txs.emplace_back(
                sle.getFieldH256(ripple::sfTokenID),
                txMeta,
                sttx.getTransactionID());
        }
    }
    // Otherwise pull it from the metadata
    else
    {
        for (ripple::STObject const& node : txMeta.getNodes())
        {
            if (node.getFieldU16(ripple::sfLedgerEntryType) !=
                ripple::ltNFTOKEN_OFFER)
                continue;

            ripple::uint256 tokenID = node.peekAtField(ripple::sfFinalFields)
                                          .downcast<ripple::STObject>()
                                          .getFieldH256(ripple::sfTokenID);
            txs.emplace_back(tokenID, txMeta, sttx.getTransactionID());
        }
    }

    // Deduplicate any transactions based on tokenID/txIdx combo. Can't just
    // use txIdx because in this case one tx can cancel offers for several
    // NFTs.
    std::sort(
        txs.begin(),
        txs.end(),
        [](NFTokenTransactionsData const& a, NFTokenTransactionsData const& b) {
            return a.tokenID < b.tokenID &&
                a.transactionIndex < b.transactionIndex;
        });
    auto last = std::unique(
        txs.begin(),
        txs.end(),
        [](NFTokenTransactionsData const& a, NFTokenTransactionsData const& b) {
            return a.tokenID == b.tokenID &&
                a.transactionIndex == b.transactionIndex;
        });
    txs.erase(last, txs.end());
    return {txs, {}};
}

// This transaction never returns an NFTokensData because it does not
// change the state of an NFT itself.
static std::
    pair<std::vector<NFTokenTransactionsData>, std::optional<NFTokensData>>
    getNFTokenCreateOfferData(
        ripple::TxMeta const& txMeta,
        ripple::STTx const& sttx)
{
    return {
        {NFTokenTransactionsData(
            sttx.getFieldH256(ripple::sfTokenID),
            txMeta,
            sttx.getTransactionID())},
        {}};
}

std::pair<std::vector<NFTokenTransactionsData>, std::optional<NFTokensData>>
getNFTokenData(
    ripple::TxMeta const& txMeta,
    ripple::STTx const& sttx,
    ripple::LedgerIndex seq,
    std::shared_ptr<BackendInterface> backend_)
{
    switch (sttx.getTxnType())
    {
        case ripple::TxType::ttNFTOKEN_MINT:
            return getNFTokenMintData(txMeta, sttx);

        case ripple::TxType::ttNFTOKEN_BURN:
            return getNFTokenBurnData(txMeta, sttx);

        case ripple::TxType::ttNFTOKEN_ACCEPT_OFFER:
            return getNFTokenAcceptOfferData(txMeta, sttx, seq, backend_);

        case ripple::TxType::ttNFTOKEN_CANCEL_OFFER:
            return getNFTokenCancelOfferData(txMeta, sttx, seq, backend_);

        case ripple::TxType::ttNFTOKEN_CREATE_OFFER:
            return getNFTokenCreateOfferData(txMeta, sttx);

        default:
            return {{}, {}};
    }
}
