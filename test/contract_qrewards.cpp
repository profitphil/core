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

    // Alice (sole holder) should be owed all 1500 QDOGE in slot 1, 0 QU in slot 0.
    uint64 pend0 = 0, pend1 = 0;
    t.getPosition(pool, QR_ALICE, pend0, pend1);
    EXPECT_EQ(pend0, 0u);
    EXPECT_EQ(pend1, 1500u);
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
