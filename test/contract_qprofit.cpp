#define NO_UEFI

#include "contract_testing.h"

// Tests for the QPROFIT ecosystem loyalty & dividend contract.
// Mirrors the harness conventions used by contract_qraffle.cpp.

static id getUser(unsigned long long i)
{
    return id(i, i / 2 + 4, i + 10, i * 3 + 8);
}

static const id QP_ADMIN = getUser(999);
static const id QP_ALICE = getUser(1);
static const id QP_BOB   = getUser(2);
static const uint64 QP_TOKEN = 123456789ULL; // arbitrary test asset name

class QProfitChecker : public QPROFIT, public QPROFIT::StateData
{
public:
    uint64 totalDistributedOf() const { return totalDistributed; }
    uint32 numAssetsOf() const { return numAssets; }
    id adminOf() const { return admin; }
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

    QProfitChecker* getState()
    {
        return (QProfitChecker*)contractStates[QPROFIT_CONTRACT_INDEX];
    }

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
        input.assetName = assetName;
        input.issuer = issuer;
        input.newOwnerAndPossessor = to;
        input.numberOfShares = n;
        invokeUserProcedure(QX_CONTRACT_INDEX, 2, input, output, from, 100);
        return output.transferredNumberOfShares;
    }

    sint32 setAdmin(const id& caller, const id& newAdmin)
    {
        QPROFIT::setAdmin_input input; input.newAdmin = newAdmin;
        QPROFIT::setAdmin_output output;
        invokeUserProcedure(QPROFIT_CONTRACT_INDEX, 7, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 registerAsset(const id& caller, uint64 assetName, const id& issuer, uint64 unit, uint32 weightBps, uint8 kind)
    {
        QPROFIT::registerAsset_input input{ assetName, issuer, unit, weightBps, kind };
        QPROFIT::registerAsset_output output;
        invokeUserProcedure(QPROFIT_CONTRACT_INDEX, 4, input, output, caller, 0);
        return output.returnCode;
    }

    void syncProfit(const id& caller, const id& user)
    {
        QPROFIT::syncProfit_input input; input.user = user;
        QPROFIT::syncProfit_output output;
        invokeUserProcedure(QPROFIT_CONTRACT_INDEX, 1, input, output, caller, 0);
    }

    uint64 claimDividends(const id& caller)
    {
        QPROFIT::claimDividends_input input;
        QPROFIT::claimDividends_output output;
        invokeUserProcedure(QPROFIT_CONTRACT_INDEX, 2, input, output, caller, 0);
        return output.paid;
    }

    void depositDividend(const id& caller, uint64 amount)
    {
        QPROFIT::depositDividend_input input;
        QPROFIT::depositDividend_output output;
        invokeUserProcedure(QPROFIT_CONTRACT_INDEX, 3, input, output, caller, amount);
    }

    uint64 getProfit(const id& user, uint64& pending)
    {
        QPROFIT::getProfit_input input; input.user = user;
        QPROFIT::getProfit_output output;
        callFunction(QPROFIT_CONTRACT_INDEX, 1, input, output);
        pending = output.pending;
        return output.profit;
    }

    uint64 previewProfit(const id& user)
    {
        QPROFIT::previewProfit_input input; input.user = user;
        QPROFIT::previewProfit_output output;
        callFunction(QPROFIT_CONTRACT_INDEX, 2, input, output);
        return output.profit;
    }
};

TEST(ContractQProfit, EntitlementAndRegistry)
{
    ContractTestingQProfit t;
    increaseEnergy(QP_ALICE, 3000000000ULL);
    // Alice issues 50,000,000 of the token to herself.
    EXPECT_EQ(t.issueAsset(QP_ALICE, QP_TOKEN, 50000000LL), 50000000LL);

    // Claim admin (claim-if-NULL bootstrap), then register the asset.
    EXPECT_EQ(t.setAdmin(QP_ADMIN, QP_ADMIN), QPROFIT_SUCCESS);
    EXPECT_EQ(t.getState()->adminOf(), QP_ADMIN);
    EXPECT_EQ(t.registerAsset(QP_ADMIN, QP_TOKEN, QP_ALICE, 1000000ULL, 10000, 0), QPROFIT_SUCCESS);
    EXPECT_EQ(t.getState()->numAssetsOf(), 1u);

    // 50 base points, x1.50 concentration band, weight 1.0x => 75 PROFIT.
    EXPECT_EQ(t.previewProfit(QP_ALICE), 75u);
    uint64 pending = 0;
    EXPECT_EQ(t.getProfit(QP_ALICE, pending), 0u); // not synced yet
    t.syncProfit(QP_ALICE, QP_ALICE);
    EXPECT_EQ(t.getProfit(QP_ALICE, pending), 75u);
    EXPECT_EQ(t.getState()->totalDistributedOf(), 75u);
}

TEST(ContractQProfit, DividendsAndReclaim)
{
    ContractTestingQProfit t;
    increaseEnergy(QP_ALICE, 3000000000ULL);
    increaseEnergy(QP_BOB, 3000000000ULL);
    t.issueAsset(QP_ALICE, QP_TOKEN, 50000000LL);
    t.setAdmin(QP_ADMIN, QP_ADMIN);
    t.registerAsset(QP_ADMIN, QP_TOKEN, QP_ALICE, 1000000ULL, 10000, 0);
    t.syncProfit(QP_ALICE, QP_ALICE);
    ASSERT_EQ(t.getState()->totalDistributedOf(), 75u);

    // Deposit 7500 QU: sole holder should be owed all of it.
    t.depositDividend(QP_BOB, 7500ULL);
    uint64 pending = 0;
    t.getProfit(QP_ALICE, pending);
    EXPECT_EQ(pending, 7500u);
    EXPECT_EQ(t.claimDividends(QP_ALICE), 7500u);
    // Nothing left to claim immediately after.
    t.getProfit(QP_ALICE, pending);
    EXPECT_EQ(pending, 0u);

    // Reclaim: Alice moves all tokens away; a resync must zero her PROFIT.
    t.transferAsset(QP_ALICE, QP_TOKEN, QP_ALICE, 50000000LL, QP_BOB);
    t.syncProfit(QP_ALICE, QP_ALICE);
    EXPECT_EQ(t.getProfit(QP_ALICE, pending), 0u);
    EXPECT_EQ(t.getState()->totalDistributedOf(), 0u);
}

TEST(ContractQProfit, AdminGating)
{
    ContractTestingQProfit t;
    increaseEnergy(QP_ALICE, 3000000000ULL);
    t.issueAsset(QP_ALICE, QP_TOKEN, 50000000LL);
    t.setAdmin(QP_ADMIN, QP_ADMIN);
    // Non-admin (Alice) cannot register assets.
    EXPECT_EQ(t.registerAsset(QP_ALICE, QP_TOKEN, QP_ALICE, 1000000ULL, 10000, 0), QPROFIT_NOT_ADMIN);
    EXPECT_EQ(t.getState()->numAssetsOf(), 0u);
}
