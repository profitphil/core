#define NO_UEFI

#include "contract_testing.h"

// Tests for the multi-pool QPROFIT ecosystem loyalty & dividend platform.

static id qpUser(unsigned long long i)
{
    return id(i, i / 2 + 4, i + 10, i * 3 + 8);
}

static const id QP_PLATFORM = qpUser(777);
static const id QP_ADMIN    = qpUser(100);
static const id QP_ALICE    = qpUser(1);
static const id QP_BOB      = qpUser(2);
static const uint64 QP_TOKEN = 123456789ULL;

class QProfitChecker : public QPROFIT, public QPROFIT::StateData
{
public:
    uint32 numPoolsOf() const { return numPools; }
    id poolAdminOf(uint64 p) const { return poolMeta.get(p).admin; }
    uint64 poolTotalDistributed(uint64 p) const { return poolMeta.get(p).totalDistributed; }
    uint32 poolNumAssets(uint64 p) const { return poolMeta.get(p).numAssets; }
};

class ContractTestingQProfit : public ContractTesting
{
public:
    ContractTestingQProfit()
    {
        initEmptySpectrum();
        initEmptyUniverse();
        system.epoch = contractDescriptions[QPROFIT_CONTRACT_INDEX].constructionEpoch;
        INIT_CONTRACT(QPROFIT);
        callSystemProcedure(QPROFIT_CONTRACT_INDEX, INITIALIZE);
        INIT_CONTRACT(QX);
        callSystemProcedure(QX_CONTRACT_INDEX, INITIALIZE);
    }

    QProfitChecker* getState() { return (QProfitChecker*)contractStates[QPROFIT_CONTRACT_INDEX]; }

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

    uint64 createPool(const id& creator, uint64 supply, uint64 fee)
    {
        QPROFIT::createPool_input input{ supply, 0 };
        QPROFIT::createPool_output output;
        invokeUserProcedure(QPROFIT_CONTRACT_INDEX, 1, input, output, creator, fee);
        return output.poolId;
    }

    sint32 registerAsset(const id& caller, uint64 poolId, uint64 assetName, const id& issuer, uint64 unit, uint32 weightBps, uint8 kind)
    {
        QPROFIT::registerAsset_input input{ poolId, assetName, issuer, unit, weightBps, kind };
        QPROFIT::registerAsset_output output;
        invokeUserProcedure(QPROFIT_CONTRACT_INDEX, 2, input, output, caller, 0);
        return output.returnCode;
    }

    void syncProfit(const id& caller, uint64 poolId, const id& user)
    {
        QPROFIT::syncProfit_input input{ poolId, user };
        QPROFIT::syncProfit_output output;
        invokeUserProcedure(QPROFIT_CONTRACT_INDEX, 6, input, output, caller, 0);
    }

    uint64 claimDividends(const id& caller, uint64 poolId)
    {
        QPROFIT::claimDividends_input input{ poolId };
        QPROFIT::claimDividends_output output;
        invokeUserProcedure(QPROFIT_CONTRACT_INDEX, 7, input, output, caller, 0);
        return output.paid;
    }

    void depositDividend(const id& caller, uint64 poolId, uint64 amount)
    {
        QPROFIT::depositDividend_input input{ poolId };
        QPROFIT::depositDividend_output output;
        invokeUserProcedure(QPROFIT_CONTRACT_INDEX, 8, input, output, caller, amount);
    }

    uint64 getPosition(uint64 poolId, const id& user, uint64& pending)
    {
        QPROFIT::getPosition_input input{ poolId, user };
        QPROFIT::getPosition_output output;
        callFunction(QPROFIT_CONTRACT_INDEX, 1, input, output);
        pending = output.pending;
        return output.profit;
    }

    uint64 previewProfit(uint64 poolId, const id& user)
    {
        QPROFIT::previewProfit_input input{ poolId, user };
        QPROFIT::previewProfit_output output;
        callFunction(QPROFIT_CONTRACT_INDEX, 2, input, output);
        return output.profit;
    }
};

TEST(ContractQProfit, CreatePoolAndEntitlement)
{
    ContractTestingQProfit t;
    increaseEnergy(QP_ADMIN, 5000000000ULL);
    increaseEnergy(QP_ALICE, 5000000000ULL);

    uint64 pool = t.createPool(QP_ADMIN, 0, QPROFIT_DEFAULT_CREATE_FEE);
    EXPECT_EQ(pool, 0u);
    EXPECT_EQ(t.getState()->numPoolsOf(), 1u);
    EXPECT_EQ(t.getState()->poolAdminOf(pool), QP_ADMIN);

    t.issueAsset(QP_ALICE, QP_TOKEN, 50000000LL);
    EXPECT_EQ(t.registerAsset(QP_ADMIN, pool, QP_TOKEN, QP_ALICE, 1000000ULL, 10000, 0), QPROFIT_SUCCESS);

    // 50 base points, x1.50 band, weight 1.0x => 75.
    EXPECT_EQ(t.previewProfit(pool, QP_ALICE), 75u);
    t.syncProfit(QP_ALICE, pool, QP_ALICE);
    uint64 pending = 0;
    EXPECT_EQ(t.getPosition(pool, QP_ALICE, pending), 75u);
    EXPECT_EQ(t.getState()->poolTotalDistributed(pool), 75u);
}

TEST(ContractQProfit, DirectedDividendsAndIsolation)
{
    ContractTestingQProfit t;
    increaseEnergy(QP_ADMIN, 10000000000ULL);
    increaseEnergy(QP_ALICE, 5000000000ULL);
    increaseEnergy(QP_BOB, 5000000000ULL);

    uint64 p0 = t.createPool(QP_ADMIN, 0, QPROFIT_DEFAULT_CREATE_FEE);
    uint64 p1 = t.createPool(QP_ADMIN, 0, QPROFIT_DEFAULT_CREATE_FEE);
    EXPECT_EQ(p1, 1u);

    t.issueAsset(QP_ALICE, QP_TOKEN, 50000000LL);
    t.registerAsset(QP_ADMIN, p0, QP_TOKEN, QP_ALICE, 1000000ULL, 10000, 0);
    t.registerAsset(QP_ADMIN, p1, QP_TOKEN, QP_ALICE, 1000000ULL, 10000, 0);
    t.syncProfit(QP_ALICE, p0, QP_ALICE);
    t.syncProfit(QP_ALICE, p1, QP_ALICE);
    ASSERT_EQ(t.getState()->poolTotalDistributed(p0), 75u);
    ASSERT_EQ(t.getState()->poolTotalDistributed(p1), 75u);

    // Deposit directed ONLY at pool 0.
    t.depositDividend(QP_BOB, p0, 7500ULL);
    uint64 pend0 = 0, pend1 = 0;
    t.getPosition(p0, QP_ALICE, pend0);
    t.getPosition(p1, QP_ALICE, pend1);
    EXPECT_EQ(pend0, 7500u); // pool 0 earns it
    EXPECT_EQ(pend1, 0u);    // pool 1 is isolated

    EXPECT_EQ(t.claimDividends(QP_ALICE, p0), 7500u);
    EXPECT_EQ(t.claimDividends(QP_ALICE, p1), 0u);
}

TEST(ContractQProfit, ReclaimAndAdminGating)
{
    ContractTestingQProfit t;
    increaseEnergy(QP_ADMIN, 5000000000ULL);
    increaseEnergy(QP_ALICE, 5000000000ULL);

    uint64 pool = t.createPool(QP_ADMIN, 0, QPROFIT_DEFAULT_CREATE_FEE);
    t.issueAsset(QP_ALICE, QP_TOKEN, 50000000LL);

    // Non-pool-admin cannot register.
    EXPECT_EQ(t.registerAsset(QP_ALICE, pool, QP_TOKEN, QP_ALICE, 1000000ULL, 10000, 0), QPROFIT_NOT_ADMIN);
    EXPECT_EQ(t.getState()->poolNumAssets(pool), 0u);

    // Admin registers; Alice earns; then she moves tokens away and a resync zeroes her.
    EXPECT_EQ(t.registerAsset(QP_ADMIN, pool, QP_TOKEN, QP_ALICE, 1000000ULL, 10000, 0), QPROFIT_SUCCESS);
    t.syncProfit(QP_ALICE, pool, QP_ALICE);
    uint64 pending = 0;
    ASSERT_EQ(t.getPosition(pool, QP_ALICE, pending), 75u);

    t.transferAsset(QP_ALICE, QP_TOKEN, QP_ALICE, 50000000LL, QP_BOB);
    t.syncProfit(QP_ALICE, pool, QP_ALICE);
    EXPECT_EQ(t.getPosition(pool, QP_ALICE, pending), 0u);
    EXPECT_EQ(t.getState()->poolTotalDistributed(pool), 0u);
}
