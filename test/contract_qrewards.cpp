#define NO_UEFI

#include "contract_testing.h"

// Tests for the multi-pool QREWARDS loyalty & dividend platform.

static id qrUser(unsigned long long i)
{
    return id(i, i / 2 + 4, i + 10, i * 3 + 8);
}

static const id QR_ADMIN = qrUser(100);
static const id QR_ALICE = qrUser(1);
static const id QR_BOB   = qrUser(2);
static const id QR_CAROL = qrUser(3); // QREWARDS shareholder (contract shares)
static const id QR_HUB   = qrUser(4); // stands in for the QPAYHUB dividends account
static const uint64 QR_TOKEN = 123456789ULL; // reward-earning asset
static const uint64 QR_DOGE  = 987654321ULL; // a dividend-currency asset

class QRewardsChecker : public QREWARDS, public QREWARDS::StateData
{
public:
    uint32 numPoolsOf() const { return numPools; }
    id poolAdminOf(uint64 p) const { return poolMeta.get(p).admin; }
    uint64 poolTotalDistributed(uint64 p) const { return poolMeta.get(p).totalDistributed; }
    uint32 poolNumAssets(uint64 p) const { return poolMeta.get(p).numAssets; }
    uint8 poolNumCurrencies(uint64 p) const { return poolMeta.get(p).numCurrencies; }
    uint64 pendingDivFeeQUOf() const { return pendingDivFeeQU; }
    id qpayTokenAddrOf() const { return qpayTokenDividendsAddress; }
};

class ContractTestingQRewards : public ContractTesting
{
public:
    ContractTestingQRewards()
    {
        initEmptySpectrum();
        initEmptyUniverse();
        system.epoch = contractDescriptions[QREWARDS_CONTRACT_INDEX].constructionEpoch;
        INIT_CONTRACT(QREWARDS);
        callSystemProcedure(QREWARDS_CONTRACT_INDEX, INITIALIZE);
        INIT_CONTRACT(QX);
        callSystemProcedure(QX_CONTRACT_INDEX, INITIALIZE);
    }

    QRewardsChecker* getState() { return (QRewardsChecker*)contractStates[QREWARDS_CONTRACT_INDEX]; }

    sint64 issueAsset(const id& issuer, uint64 assetName, sint64 numberOfShares)
    {
        QX::IssueAsset_input input{ assetName, numberOfShares, 0, 0 };
        QX::IssueAsset_output output;
        invokeUserProcedure(QX_CONTRACT_INDEX, 1, input, output, issuer, 1000000000ULL);
        return output.issuedNumberOfShares;
    }

    sint64 transferAsset(const id& issuer, uint64 assetName, const id& from, sint64 n, const id& to)
    {
        QX::TransferShareOwnershipAndPossession_input input;
        QX::TransferShareOwnershipAndPossession_output output;
        input.assetName = assetName; input.issuer = issuer;
        input.newOwnerAndPossessor = to; input.numberOfShares = n;
        invokeUserProcedure(QX_CONTRACT_INDEX, 2, input, output, from, 100);
        return output.transferredNumberOfShares;
    }

    sint64 grantMgmtToQRewards(const id& issuer, uint64 assetName, const id& owner, sint64 n)
    {
        QX::TransferShareManagementRights_input input;
        QX::TransferShareManagementRights_output output;
        input.asset.assetName = assetName; input.asset.issuer = issuer;
        input.newManagingContractIndex = QREWARDS_CONTRACT_INDEX;
        input.numberOfShares = n;
        invokeUserProcedure(QX_CONTRACT_INDEX, 9, input, output, owner, 0);
        return output.transferredNumberOfShares;
    }

    uint64 createPool(const id& creator, uint64 supply, uint64 fee)
    {
        QREWARDS::createPool_input input{ supply, 0 };
        QREWARDS::createPool_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 1, input, output, creator, fee);
        return output.poolId;
    }

    sint32 registerAsset(const id& caller, uint64 poolId, uint64 assetName, const id& issuer, uint64 unit, uint32 weightBps, uint8 kind)
    {
        QREWARDS::registerAsset_input input{ poolId, assetName, issuer, unit, weightBps, kind };
        QREWARDS::registerAsset_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 2, input, output, caller, 0);
        return output.returnCode;
    }

    uint32 registerAssetsBatch(const id& caller, uint64 poolId, uint32 count, const id& issuer, uint64 unit)
    {
        QREWARDS::registerAssets_input input;
        memset(&input, 0, sizeof(input));
        input.poolId = poolId; input.count = count;
        for (uint32 i = 0; i < count; i++)
        {
            QREWARDS::AssetSpec s;
            s.assetName = QR_TOKEN; s.issuer = issuer; s.unit = unit; s.weightBps = 10000; s.kind = 0;
            input.specs.set(i, s);
        }
        QREWARDS::registerAssets_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 12, input, output, caller, 0);
        return output.added;
    }

    sint32 addDivCurrency(const id& caller, uint64 poolId, uint64 assetName, const id& issuer)
    {
        QREWARDS::addDividendCurrency_input input{ poolId, assetName, issuer };
        QREWARDS::addDividendCurrency_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 13, input, output, caller, 0);
        return output.returnCode;
    }

    void syncProfit(const id& caller, uint64 poolId, const id& user)
    {
        QREWARDS::syncProfit_input input{ poolId, user };
        QREWARDS::syncProfit_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 6, input, output, caller, 0);
    }

    uint32 claimDividends(const id& caller, uint64 poolId)
    {
        QREWARDS::claimDividends_input input{ poolId };
        QREWARDS::claimDividends_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 7, input, output, caller, 0);
        return output.currenciesPaid;
    }

    void depositQU(const id& caller, uint64 poolId, uint64 amount)
    {
        QREWARDS::depositDividend_input input{ poolId, 0, 0 };
        QREWARDS::depositDividend_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 8, input, output, caller, amount);
    }

    sint32 depositAsset(const id& caller, uint64 poolId, uint32 currencyIndex, uint64 amount)
    {
        QREWARDS::depositDividend_input input{ poolId, currencyIndex, amount };
        QREWARDS::depositDividend_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 8, input, output, caller, 0);
        return output.returnCode;
    }

    uint64 getPosition(uint64 poolId, const id& user, uint64& pending0, uint64& pending1)
    {
        QREWARDS::getPosition_input input{ poolId, user };
        QREWARDS::getPosition_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 1, input, output);
        pending0 = output.pending.get(0);
        pending1 = output.pending.get(1);
        return output.profit;
    }

    uint32 getAllPoolAssets(uint64 poolId)
    {
        QREWARDS::getAllPoolAssets_input input{ poolId };
        QREWARDS::getAllPoolAssets_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 6, input, output);
        return output.count;
    }

    sint32 setFundingRoute(const id& caller, uint64 poolId, bit clear)
    {
        QREWARDS::setFundingRoute_input input{ poolId, clear };
        QREWARDS::setFundingRoute_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 14, input, output, caller, 0);
        return output.returnCode;
    }

    uint64 getFundingRoute(const id& source, bit& isSet)
    {
        QREWARDS::getFundingRoute_input input{ source };
        QREWARDS::getFundingRoute_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 7, input, output);
        isSet = output.isSet;
        return output.poolId;
    }

    uint32 getPoolsByAdmin(const id& admin, uint32 offset, uint64& first, uint32& total)
    {
        QREWARDS::getPoolsByAdmin_input input{ admin, offset };
        QREWARDS::getPoolsByAdmin_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 8, input, output);
        first = output.poolIds.get(0);
        total = output.totalMatched;
        return output.count;
    }

    uint32 getPositions2(const id& user, uint64 pA, uint64 pB, uint64& profitA, uint8& validB)
    {
        QREWARDS::getPositions_input input;
        memset(&input, 0, sizeof(input));
        input.user = user; input.count = 2;
        input.poolIds.set(0, pA); input.poolIds.set(1, pB);
        QREWARDS::getPositions_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 9, input, output);
        profitA = output.positions.get(0).profit;
        validB = output.positions.get(1).valid;
        return output.count;
    }

    sint32 depositOperating(const id& caller, uint64 poolId, uint64 amount)
    {
        QREWARDS::depositOperating_input input{ poolId };
        QREWARDS::depositOperating_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 15, input, output, caller, amount);
        return output.returnCode;
    }

    void endEpoch()
    {
        callSystemProcedure(QREWARDS_CONTRACT_INDEX, END_EPOCH);
    }

    uint8 getPoolState(uint64 poolId, uint64& operatingBalance, uint8& paused)
    {
        QREWARDS::getPool_input input{ poolId };
        QREWARDS::getPool_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 3, input, output);
        operatingBalance = output.operatingBalance;
        paused = output.paused;
        return output.active;
    }

    uint64 previewProfit(uint64 poolId, const id& user)
    {
        QREWARDS::previewProfit_input input{ poolId, user };
        QREWARDS::previewProfit_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 2, input, output);
        return output.profit;
    }

    sint32 setExcluded(const id& caller, uint64 poolId, const id& address, bit excluded)
    {
        QREWARDS::setExcludedAddress_input input{ poolId, address, excluded };
        QREWARDS::setExcludedAddress_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 17, input, output, caller, 0);
        return output.returnCode;
    }

    bit isExcluded(uint64 poolId, const id& address)
    {
        QREWARDS::isExcluded_input input{ poolId, address };
        QREWARDS::isExcluded_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 10, input, output);
        return output.excluded;
    }

    sint32 setPlatformOwner(const id& caller, const id& newOwner)
    {
        QREWARDS::setPlatformOwner_input input{ newOwner };
        QREWARDS::setPlatformOwner_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 10, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 setQpayhubAddr(const id& caller, const id& addr)
    {
        QREWARDS::setQpayhubAddress_input input{ addr };
        QREWARDS::setQpayhubAddress_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 16, input, output, caller, 0);
        return output.returnCode;
    }

    // Sum the shares of (assetName, issuer) possessed by `holder`, across any managing contract.
    uint64 tokenBalanceOf(uint64 assetName, const id& issuer, const id& holder)
    {
        Asset asset(issuer, assetName);
        uint64 total = 0;
        for (AssetPossessionIterator iter(asset); !iter.reachedEnd(); iter.next())
        {
            if (iter.possessor() == holder)
                total += (uint64)iter.numberOfPossessedShares();
        }
        return total;
    }
};

TEST(ContractQRewards, CreatePoolAndEntitlement)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 5000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    EXPECT_EQ(pool, 0u);
    EXPECT_EQ(t.getState()->numPoolsOf(), 1u);
    EXPECT_EQ(t.getState()->poolAdminOf(pool), QR_ADMIN);
    EXPECT_EQ(t.getState()->poolNumCurrencies(pool), 1u); // QU by default

    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    EXPECT_EQ(t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0), QREWARDS_SUCCESS);

    t.syncProfit(QR_ALICE, pool, QR_ALICE);
    uint64 p0 = 0, p1 = 0;
    EXPECT_EQ(t.getPosition(pool, QR_ALICE, p0, p1), 75u);
    EXPECT_EQ(t.getState()->poolTotalDistributed(pool), 75u);
}

TEST(ContractQRewards, DirectedQuDividendsAndIsolation)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);

    uint64 a = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    uint64 b = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, a, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    t.registerAsset(QR_ADMIN, b, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    t.syncProfit(QR_ALICE, a, QR_ALICE);
    t.syncProfit(QR_ALICE, b, QR_ALICE);

    t.depositQU(QR_BOB, a, 7500ULL); // directed at pool a only
    uint64 pa0 = 0, pa1 = 0, pb0 = 0, pb1 = 0;
    t.getPosition(a, QR_ALICE, pa0, pa1);
    t.getPosition(b, QR_ALICE, pb0, pb1);
    EXPECT_EQ(pa0, 7500u);
    EXPECT_EQ(pb0, 0u); // isolated
    EXPECT_EQ(t.claimDividends(QR_ALICE, a), 1u);
    EXPECT_EQ(t.claimDividends(QR_ALICE, b), 0u);
}

TEST(ContractQRewards, BatchRegisterAndListAssets)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 5000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);

    // Batch of 5 (all the same issued asset here, for test simplicity).
    EXPECT_EQ(t.registerAssetsBatch(QR_ADMIN, pool, 5, QR_ALICE, 1000000ULL), 5u);
    EXPECT_EQ(t.getState()->poolNumAssets(pool), 5u);
    EXPECT_EQ(t.getAllPoolAssets(pool), 5u);
}

TEST(ContractQRewards, AssetDividendAccounting)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 5000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    t.syncProfit(QR_ALICE, pool, QR_ALICE); // Alice => 75
    ASSERT_EQ(t.getState()->poolTotalDistributed(pool), 75u);

    // Add QDOGE as a second dividend currency (slot 1).
    t.issueAsset(QR_BOB, QR_DOGE, 1000000LL);
    EXPECT_EQ(t.addDivCurrency(QR_ADMIN, pool, QR_DOGE, QR_BOB), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->poolNumCurrencies(pool), 2u);

    // Bob grants management of QDOGE to QREWARDS, then deposits 1500 QDOGE to slot 1.
    t.grantMgmtToQRewards(QR_BOB, QR_DOGE, QR_BOB, 1500LL);
    EXPECT_EQ(t.depositAsset(QR_BOB, pool, 1, 1500ULL), QREWARDS_SUCCESS);

    // 5% token dividend fee applies: 1500 -> 75 fee, 1425 reaches the sole holder.
    // No QREWARDS shareholders are seeded in this fixture, so the 20% shareholder leg
    // rounds/falls back entirely to the QPAY token dividends address (whole 75).
    uint64 pend0 = 0, pend1 = 0;
    t.getPosition(pool, QR_ALICE, pend0, pend1);
    EXPECT_EQ(pend0, 0u);
    EXPECT_EQ(pend1, 1425u);
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_BOB, t.getState()->qpayTokenAddrOf()), 75u);
    EXPECT_EQ(t.claimDividends(QR_ALICE, pool), 1u); // paid in QDOGE
}

TEST(ContractQRewards, ReclaimAndAdminGating)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 5000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);

    EXPECT_EQ(t.registerAsset(QR_ALICE, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0), QREWARDS_NOT_ADMIN);
    EXPECT_EQ(t.getState()->poolNumAssets(pool), 0u);

    EXPECT_EQ(t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0), QREWARDS_SUCCESS);
    t.syncProfit(QR_ALICE, pool, QR_ALICE);
    uint64 p0 = 0, p1 = 0;
    ASSERT_EQ(t.getPosition(pool, QR_ALICE, p0, p1), 75u);

    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 50000000LL, QR_BOB);
    t.syncProfit(QR_ALICE, pool, QR_ALICE);
    EXPECT_EQ(t.getPosition(pool, QR_ALICE, p0, p1), 0u);
    EXPECT_EQ(t.getState()->poolTotalDistributed(pool), 0u);
}

TEST(ContractQRewards, FundingRouteTable)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);

    bit isSet = 0;
    t.getFundingRoute(QR_BOB, isSet);
    EXPECT_EQ(isSet, 0); // no route yet

    // Can't route to a non-existent pool.
    EXPECT_EQ(t.setFundingRoute(QR_BOB, 99, 0), QREWARDS_POOL_NOT_FOUND);

    // Register a route, then read it back.
    EXPECT_EQ(t.setFundingRoute(QR_BOB, pool, 0), QREWARDS_SUCCESS);
    uint64 routed = t.getFundingRoute(QR_BOB, isSet);
    EXPECT_EQ(isSet, 1);
    EXPECT_EQ(routed, pool);

    // Clear it.
    EXPECT_EQ(t.setFundingRoute(QR_BOB, 0, 1), QREWARDS_SUCCESS);
    t.getFundingRoute(QR_BOB, isSet);
    EXPECT_EQ(isSet, 0);
}

TEST(ContractQRewards, GetPoolsByAdmin)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_BOB, 10000000000ULL);

    t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE); // 0
    t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE); // 1
    t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE); // 2
    uint64 bobPool = t.createPool(QR_BOB, 0, QREWARDS_DEFAULT_CREATE_FEE); // 3

    uint64 first = 0; uint32 total = 0;
    uint32 cnt = t.getPoolsByAdmin(QR_ADMIN, 0, first, total);
    EXPECT_EQ(total, 3u);
    EXPECT_EQ(cnt, 3u);
    EXPECT_EQ(first, 0u);

    cnt = t.getPoolsByAdmin(QR_BOB, 0, first, total);
    EXPECT_EQ(total, 1u);
    EXPECT_EQ(cnt, 1u);
    EXPECT_EQ(first, bobPool);
}

TEST(ContractQRewards, OperatingFeePauseDeactivate)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);

    // 5M create fee + 250k seeded into operating balance (the excess).
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE + 250000ULL);
    uint64 bal = 0; uint8 paused = 9;
    EXPECT_EQ(t.getPoolState(pool, bal, paused), 1);
    EXPECT_EQ(bal, 250000u);
    EXPECT_EQ(paused, 0);

    // Two epochs draw 100k each; still funded.
    t.endEpoch();
    t.getPoolState(pool, bal, paused);
    EXPECT_EQ(bal, 150000u); EXPECT_EQ(paused, 0);
    t.endEpoch();
    t.getPoolState(pool, bal, paused);
    EXPECT_EQ(bal, 50000u); EXPECT_EQ(paused, 0);

    // Now underfunded: paused for 2 epochs (still active)...
    t.endEpoch();
    EXPECT_EQ(t.getPoolState(pool, bal, paused), 1); EXPECT_EQ(paused, 1);
    t.endEpoch();
    EXPECT_EQ(t.getPoolState(pool, bal, paused), 1); EXPECT_EQ(paused, 1);
    // ...then deactivated on the next miss.
    t.endEpoch();
    EXPECT_EQ(t.getPoolState(pool, bal, paused), 0); // deactivated

    // Topping up a deactivated pool is rejected.
    EXPECT_EQ(t.depositOperating(QR_ADMIN, pool, 1000000ULL), QREWARDS_POOL_INACTIVE);
}

TEST(ContractQRewards, GetPositionsBatch)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);

    uint64 a = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE); // 0
    t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);            // 1
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, a, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    t.syncProfit(QR_ALICE, a, QR_ALICE); // Alice earns 75 in pool a only

    uint64 profitA = 0; uint8 validForNonexistent = 9;
    // Query pool a (has profit) and pool 99 (does not exist).
    uint32 cnt = t.getPositions2(QR_ALICE, a, 99, profitA, validForNonexistent);
    EXPECT_EQ(cnt, 2u);
    EXPECT_EQ(profitA, 75u);
    EXPECT_EQ(validForNonexistent, 0); // pool 99 flagged invalid
}

TEST(ContractQRewards, ExcludedAddress)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    EXPECT_EQ(t.previewProfit(pool, QR_ALICE), 75u);

    // Exclude Alice -> entitlement 0, synced to zero, removed from totals.
    EXPECT_EQ(t.setExcluded(QR_ADMIN, pool, QR_ALICE, 1), QREWARDS_SUCCESS);
    EXPECT_EQ(t.isExcluded(pool, QR_ALICE), 1);
    EXPECT_EQ(t.previewProfit(pool, QR_ALICE), 0u);
    t.syncProfit(QR_ALICE, pool, QR_ALICE);
    uint64 p0 = 0, p1 = 0;
    EXPECT_EQ(t.getPosition(pool, QR_ALICE, p0, p1), 0u);
    EXPECT_EQ(t.getState()->poolTotalDistributed(pool), 0u);

    // Re-include -> earns again.
    EXPECT_EQ(t.setExcluded(QR_ADMIN, pool, QR_ALICE, 0), QREWARDS_SUCCESS);
    EXPECT_EQ(t.isExcluded(pool, QR_ALICE), 0);
    EXPECT_EQ(t.previewProfit(pool, QR_ALICE), 75u);

    // Non-admin cannot exclude.
    EXPECT_EQ(t.setExcluded(QR_ALICE, pool, QR_BOB, 1), QREWARDS_NOT_ADMIN);
}

TEST(ContractQRewards, DividendFeeFivePercent)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    t.syncProfit(QR_ALICE, pool, QR_ALICE);
    ASSERT_EQ(t.getState()->poolTotalDistributed(pool), 75u);

    // 10,000 QU deposited: 5% (500) fee accrues, 9,500 reaches the sole holder.
    t.depositQU(QR_BOB, pool, 10000ULL);
    uint64 p0 = 0, p1 = 0;
    t.getPosition(pool, QR_ALICE, p0, p1);
    EXPECT_EQ(p0, 9500u);
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 500u);

    // END_EPOCH flushes the accrued fee (80/20).
    t.endEpoch();
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 0u);
}

// Token dividend fee is split like the QU fee: 20% to QREWARDS shareholders (paid in the
// token itself, since distributeDividends is QU-only), 80% (+dust) to the QPAY address.
TEST(ContractQRewards, TokenDividendFeeSplit)
{
    ContractTestingQRewards t;

    // Seed QREWARDS shareholders: Carol owns/possesses all contract shares.
    std::vector<std::pair<m256i, unsigned int>> owners = { { QR_CAROL, NUMBER_OF_COMPUTORS } };
    issueContractShares(QREWARDS_CONTRACT_INDEX, owners, false);

    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    t.syncProfit(QR_ALICE, pool, QR_ALICE); // Alice => 75 reward tokens (sole holder)
    ASSERT_EQ(t.getState()->poolTotalDistributed(pool), 75u);

    // QDOGE as a second dividend currency (slot 1).
    t.issueAsset(QR_BOB, QR_DOGE, 5000000LL);
    EXPECT_EQ(t.addDivCurrency(QR_ADMIN, pool, QR_DOGE, QR_BOB), QREWARDS_SUCCESS);

    // Deposit 150,000 QDOGE. 5% fee = 7,500.
    //   shareholder leg = 20% of 7,500 = 1,500; perShare = 1500/676 = 2; distributed = 2*676 = 1,352 -> Carol.
    //   QPAY leg = 7,500 - 1,352 = 6,148 -> QPAY token dividends address.
    //   net to the pool = 142,500 -> owed to Alice (142500 = 75*1900, exact).
    t.grantMgmtToQRewards(QR_BOB, QR_DOGE, QR_BOB, 150000LL);
    EXPECT_EQ(t.depositAsset(QR_BOB, pool, 1, 150000ULL), QREWARDS_SUCCESS);

    uint64 pend0 = 0, pend1 = 0;
    t.getPosition(pool, QR_ALICE, pend0, pend1);
    EXPECT_EQ(pend0, 0u);
    EXPECT_EQ(pend1, 142500u);
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_BOB, QR_CAROL), 1352u);
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_BOB, t.getState()->qpayTokenAddrOf()), 6148u);
}

// QU dividend fee routing: 20% -> QREWARDS shareholders (distributeDividends),
// 80% (+rounding) -> the QPAYHUB dividends account (qpayhubAddress), NOT the QPAY wallet.
TEST(ContractQRewards, QuDividendFeeRouting)
{
    ContractTestingQRewards t;

    // Carol owns/possesses all contract shares.
    std::vector<std::pair<m256i, unsigned int>> owners = { { QR_CAROL, NUMBER_OF_COMPUTORS } };
    issueContractShares(QREWARDS_CONTRACT_INDEX, owners, false);

    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);

    // Claim platform ownership (bootstrap from NULL) and set the QPAYHUB dividends account.
    EXPECT_EQ(t.setPlatformOwner(QR_ADMIN, QR_ADMIN), QREWARDS_SUCCESS);
    EXPECT_EQ(t.setQpayhubAddr(QR_ADMIN, QR_HUB), QREWARDS_SUCCESS);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    t.syncProfit(QR_ALICE, pool, QR_ALICE);

    // Snapshot AFTER createPool so the create-fee split (which also pays QR_HUB and Carol)
    // is in the baseline; the deltas below isolate the dividend-fee legs.
    long long carolBefore = getBalance(QR_CAROL);
    long long hubBefore = getBalance(QR_HUB);

    // Deposit 1,352,000 QU: fee = 67,600. Shareholder leg = 20% = 13,520; perShare = 20;
    // distributed = 20*676 = 13,520 -> Carol. QPAYHUB leg = 67,600 - 13,520 = 54,080 -> QR_HUB.
    t.depositQU(QR_BOB, pool, 1352000ULL);
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 67600u);
    t.endEpoch();
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 0u);

    EXPECT_EQ(getBalance(QR_CAROL) - carolBefore, 13520LL); // 20% to shareholders
    EXPECT_EQ(getBalance(QR_HUB) - hubBefore, 54080LL);     // 80% to QPAYHUB account
}
