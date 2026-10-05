#define NO_UEFI

#include "contract_testing.h"

// Tests for the QREWARDS loyalty & dividend platform (pure-snapshot model).
//
// Dividends accumulate into per-currency pots and are distributed at END_EPOCH by
// LIVE weighted holdings (read straight from the asset ledger). There is no claim
// step, no persistent reward-token balance, and no stale "ghost" positions.

static id qrUser(unsigned long long i)
{
    return id(i, i / 2 + 4, i + 10, i * 3 + 8);
}

static const id QR_ADMIN = qrUser(100);
static const id QR_ALICE = qrUser(1);
static const id QR_BOB   = qrUser(2);
static const id QR_CAROL = qrUser(3); // QREWARDS shareholder (contract shares)
static const id QR_HUB   = qrUser(4); // stands in for the QPAYHUB dividends account
static const id QR_FUND  = qrUser(5); // neutral depositor (never a pool holder)
static const uint64 QR_TOKEN = 123456789ULL; // reward-earning asset
static const uint64 QR_DOGE  = 987654321ULL; // a dividend-currency asset

class QRewardsChecker : public QREWARDS, public QREWARDS::StateData
{
public:
    uint32 numPoolsOf() const { return numPools; }
    id poolAdminOf(uint64 p) const { return poolMeta.get(p).admin; }
    uint32 poolNumAssets(uint64 p) const { return poolMeta.get(p).numAssets; }
    uint8 poolNumCurrencies(uint64 p) const { return poolMeta.get(p).numCurrencies; }
    uint8 poolActive(uint64 p) const { return poolMeta.get(p).active; }
    uint8 poolPaused(uint64 p) const { return poolMeta.get(p).paused; }
    uint64 poolLastTotalWeight(uint64 p) const { return poolMeta.get(p).lastTotalWeight; }
    uint64 poolPot(uint64 p, uint32 k) const { return poolMeta.get(p).currencies.get(k).pot; }
    uint64 poolDistributable(uint64 p, uint32 k) const { return poolMeta.get(p).currencies.get(k).distributable; }
    uint64 pendingDivFeeQUOf() const { return pendingDivFeeQU; }
    id qpayTokenAddrOf() const { return qpayTokenDividendsAddress; }
    uint8 distributionModeOf() const { return distributionMode; }
    uint8 cycleActiveOf() const { return cycleActive; }
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

    uint64 createPool(const id& creator, uint64 label, uint64 fee)
    {
        QREWARDS::createPool_input input{ label };
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
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 11, input, output, caller, 0);
        return output.added;
    }

    sint32 addDivCurrency(const id& caller, uint64 poolId, uint64 assetName, const id& issuer)
    {
        QREWARDS::addDividendCurrency_input input{ poolId, assetName, issuer };
        QREWARDS::addDividendCurrency_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 12, input, output, caller, 0);
        return output.returnCode;
    }

    void depositQU(const id& caller, uint64 poolId, uint64 amount)
    {
        QREWARDS::depositDividend_input input{ poolId, 0, 0 };
        QREWARDS::depositDividend_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 6, input, output, caller, amount);
    }

    sint32 depositAsset(const id& caller, uint64 poolId, uint32 currencyIndex, uint64 amount)
    {
        QREWARDS::depositDividend_input input{ poolId, currencyIndex, amount };
        QREWARDS::depositDividend_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 6, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 depositOperating(const id& caller, uint64 poolId, uint64 amount)
    {
        QREWARDS::depositOperating_input input{ poolId };
        QREWARDS::depositOperating_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 7, input, output, caller, amount);
        return output.returnCode;
    }

    sint32 setPlatformParams(const id& caller, uint64 createFee, uint64 operatingFee)
    {
        QREWARDS::setPlatformParams_input input{ createFee, operatingFee };
        QREWARDS::setPlatformParams_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 8, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 setPlatformOwner(const id& caller, const id& newOwner)
    {
        QREWARDS::setPlatformOwner_input input{ newOwner };
        QREWARDS::setPlatformOwner_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 9, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 setFundingRoute(const id& caller, uint64 poolId, bit clear)
    {
        QREWARDS::setFundingRoute_input input{ poolId, clear };
        QREWARDS::setFundingRoute_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 13, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 setQpayhubAddr(const id& caller, const id& addr)
    {
        QREWARDS::setQpayhubAddress_input input{ addr };
        QREWARDS::setQpayhubAddress_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 14, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 setExcluded(const id& caller, uint64 poolId, const id& address, bit excluded)
    {
        QREWARDS::setExcludedAddress_input input{ poolId, address, excluded };
        QREWARDS::setExcludedAddress_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 15, input, output, caller, 0);
        return output.returnCode;
    }

    void endEpoch()
    {
        callSystemProcedure(QREWARDS_CONTRACT_INDEX, END_EPOCH);
    }

    void beginEpoch()
    {
        callSystemProcedure(QREWARDS_CONTRACT_INDEX, BEGIN_EPOCH);
    }

    void endTick()
    {
        callSystemProcedure(QREWARDS_CONTRACT_INDEX, END_TICK);
    }

    sint32 setDistributionMode(const id& caller, uint8 mode)
    {
        QREWARDS::setDistributionMode_input input{ mode };
        QREWARDS::setDistributionMode_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 17, input, output, caller, 0);
        return output.returnCode;
    }

    sint32 setStreamParams(const id& caller, uint32 delayTicks, uint32 batchSize)
    {
        QREWARDS::setStreamParams_input input{ delayTicks, batchSize };
        QREWARDS::setStreamParams_output output;
        invokeUserProcedure(QREWARDS_CONTRACT_INDEX, 18, input, output, caller, 0);
        return output.returnCode;
    }

    // Enable streamed mode with no start delay and a given batch size (owner = QR_ADMIN).
    void enableStreamed(uint32 batchSize)
    {
        EXPECT_EQ(setPlatformOwner(QR_ADMIN, QR_ADMIN), QREWARDS_SUCCESS);
        EXPECT_EQ(setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, 0ULL), QREWARDS_SUCCESS); // no operating fee
        EXPECT_EQ(setDistributionMode(QR_ADMIN, QREWARDS_DIST_STREAMED), QREWARDS_SUCCESS);
        EXPECT_EQ(setStreamParams(QR_ADMIN, 0u, batchSize), QREWARDS_SUCCESS);
    }

    // Simulate a plain (standard) QU transfer landing on the contract address, which the
    // kernel delivers to POST_INCOMING_TRANSFER. (invokeUserProcedure only ever fires the
    // procedureTransaction type, which the funding-route path ignores.)
    void simulateIncomingTransfer(const id& from, sint64 amount)
    {
        increaseEnergy(id(QREWARDS_CONTRACT_INDEX, 0, 0, 0), amount);
        QpiContextSystemProcedureCall qpiContext(QREWARDS_CONTRACT_INDEX, POST_INCOMING_TRANSFER);
        QPI::PostIncomingTransfer_input input{ from, amount, QPI::TransferType::standardTransaction };
        qpiContext.call(input);
    }

    uint64 previewWeight(uint64 poolId, const id& user)
    {
        QREWARDS::previewWeight_input input{ poolId, user };
        QREWARDS::previewWeight_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 8, input, output);
        return output.weight;
    }

    uint64 getPoolPot(uint64 poolId, uint32 k, uint64& lastTotalWeight, uint8& paused)
    {
        QREWARDS::getPool_input input{ poolId };
        QREWARDS::getPool_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 1, input, output);
        lastTotalWeight = output.lastTotalWeight;
        paused = output.paused;
        return output.currencyPot.get(k);
    }

    uint32 getAllPoolAssets(uint64 poolId)
    {
        QREWARDS::getAllPoolAssets_input input{ poolId };
        QREWARDS::getAllPoolAssets_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 3, input, output);
        return output.count;
    }

    uint32 getPoolsByAdmin(const id& admin, uint32 offset, uint64& first, uint32& total)
    {
        QREWARDS::getPoolsByAdmin_input input{ admin, offset };
        QREWARDS::getPoolsByAdmin_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 6, input, output);
        first = output.poolIds.get(0);
        total = output.totalMatched;
        return output.count;
    }

    bit isExcluded(uint64 poolId, const id& address)
    {
        QREWARDS::isExcluded_input input{ poolId, address };
        QREWARDS::isExcluded_output output;
        callFunction(QREWARDS_CONTRACT_INDEX, 7, input, output);
        return output.excluded;
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

    // Claim platform ownership and disable the operating fee so distribution tests
    // are not perturbed by pool pausing.
    void disableOperatingFee()
    {
        EXPECT_EQ(setPlatformOwner(QR_ADMIN, QR_ADMIN), QREWARDS_SUCCESS);
        EXPECT_EQ(setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, 0ULL), QREWARDS_SUCCESS);
    }
};

TEST(ContractQRewards, CreatePoolAndWeight)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    EXPECT_EQ(t.getState()->numPoolsOf(), 1u);
    EXPECT_EQ(t.getState()->poolAdminOf(pool), QR_ADMIN);

    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    EXPECT_EQ(t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0), QREWARDS_SUCCESS);

    // 50,000,000 / 1,000,000 = 50 pts -> x1.5 multiplier -> weight 75.
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 75u);
    EXPECT_EQ(t.previewWeight(pool, QR_BOB), 0u);
}

TEST(ContractQRewards, QuDividendDistributedAtEpoch)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);

    // Deposit 10,000 QU: 5% (500) fee, 9,500 into the pot; Alice (sole holder) gets it all.
    t.depositQU(QR_FUND, pool, 10000ULL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u);
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 500u);

    long long aliceBefore = getBalance(QR_ALICE);
    t.endEpoch();
    EXPECT_EQ(getBalance(QR_ALICE) - aliceBefore, 9500LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 75u);
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 0u); // fee flushed
}

// THE KEY TEST: holder 1 earns in epoch 1, sells, holder 2 buys, epoch 2 pays only
// holder 2 -- the same tokens are never counted twice and the seller gets nothing.
TEST(ContractQRewards, SellThenRebuyNoDoubleCount)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL); // Alice issues & holds all 50M
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);

    // --- Epoch 1: Alice is the sole holder (weight 75). ---
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500
    long long aliceStart = getBalance(QR_ALICE);
    long long bobStart = getBalance(QR_BOB);
    t.endEpoch();
    EXPECT_EQ(getBalance(QR_ALICE) - aliceStart, 9500LL); // Alice paid
    EXPECT_EQ(getBalance(QR_BOB) - bobStart, 0LL);

    // --- Alice sells ALL her tokens to Bob (contract is never told). ---
    EXPECT_EQ(t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 50000000LL, QR_BOB), 50000000LL);
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 0u);  // Alice now weightless
    EXPECT_EQ(t.previewWeight(pool, QR_BOB), 75u);   // Bob now has the weight

    // --- Epoch 2: deposit again. Only Bob should be paid; Alice gets nothing. ---
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500
    long long aliceMid = getBalance(QR_ALICE);
    long long bobMid = getBalance(QR_BOB);
    t.endEpoch();

    EXPECT_EQ(getBalance(QR_BOB) - bobMid, 9500LL);   // Bob gets the FULL 9500
    EXPECT_EQ(getBalance(QR_ALICE) - aliceMid, 0LL);  // the seller gets NOTHING
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 75u); // not 150 -> no double count
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
}

TEST(ContractQRewards, ProRataSplitBetweenHolders)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    // unit 1M, weight 1x. Alice 50M -> 50 pts -> x1.5 -> 75. Bob 50M via issue too.
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    // Give Bob 50M of the same asset (Alice transfers half? issue is one-issuer). Alice sends 25M to Bob.
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_BOB);
    // Alice now 25M -> 25 pts -> x1.25 -> weight = 25*12500/10000 = 31.
    // Bob 25M -> 25 pts -> x1.25 -> weight 31. total 62.
    EXPECT_EQ(t.previewWeight(pool, QR_ALICE), 31u);
    EXPECT_EQ(t.previewWeight(pool, QR_BOB), 31u);

    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500, total weight 62
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
    t.endEpoch();
    // each gets floor(9500 * 31 / 62) = floor(4750) = 4750.
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 4750LL);
    EXPECT_EQ(getBalance(QR_BOB) - b0, 4750LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u); // 9500 fully distributed
}

TEST(ContractQRewards, ExcludedHolderGetsNothing)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_BOB); // Alice 25M, Bob 25M

    // Exclude Bob: Alice should take the whole pot, Bob zero, and Bob must not dilute.
    EXPECT_EQ(t.setExcluded(QR_ADMIN, pool, QR_BOB, 1), QREWARDS_SUCCESS);
    EXPECT_EQ(t.isExcluded(pool, QR_BOB), 1);
    EXPECT_EQ(t.previewWeight(pool, QR_BOB), 0u);

    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);
    t.endEpoch();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL); // Alice gets all (Bob excluded)
    EXPECT_EQ(getBalance(QR_BOB) - b0, 0LL);
    EXPECT_EQ(t.getState()->poolLastTotalWeight(pool), 31u); // only Alice's 31 counted
}

TEST(ContractQRewards, TokenDividendDistributedAtEpoch)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0); // Alice weight 75

    // QDOGE as currency 1, funded by QR_FUND.
    t.issueAsset(QR_FUND, QR_DOGE, 5000000LL);
    EXPECT_EQ(t.addDivCurrency(QR_ADMIN, pool, QR_DOGE, QR_FUND), QREWARDS_SUCCESS);

    // Deposit 150,000 QDOGE: 5% (7,500) token fee -> QPAY wallet (no QREWARDS shareholders
    // seeded, so the 20% shareholder leg folds to QPAY). 142,500 into the pot.
    t.grantMgmtToQRewards(QR_FUND, QR_DOGE, QR_FUND, 150000LL);
    EXPECT_EQ(t.depositAsset(QR_FUND, pool, 1, 150000ULL), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->poolPot(pool, 1), 142500u);
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_FUND, t.getState()->qpayTokenAddrOf()), 7500u);

    t.endEpoch();
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_FUND, QR_ALICE), 142500u); // Alice (sole holder) paid in QDOGE
    EXPECT_EQ(t.getState()->poolPot(pool, 1), 0u);
}

// QU dividend fee routing: 20% -> QREWARDS shareholders, 80% (+rounding) -> QPAYHUB account.
TEST(ContractQRewards, QuDividendFeeRouting)
{
    ContractTestingQRewards t;

    std::vector<std::pair<m256i, unsigned int>> owners = { { QR_CAROL, NUMBER_OF_COMPUTORS } };
    issueContractShares(QREWARDS_CONTRACT_INDEX, owners, false);

    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    EXPECT_EQ(t.setPlatformOwner(QR_ADMIN, QR_ADMIN), QREWARDS_SUCCESS);
    EXPECT_EQ(t.setPlatformParams(QR_ADMIN, QREWARDS_DEFAULT_CREATE_FEE, 0ULL), QREWARDS_SUCCESS);
    EXPECT_EQ(t.setQpayhubAddr(QR_ADMIN, QR_HUB), QREWARDS_SUCCESS);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);

    long long carolBefore = getBalance(QR_CAROL);
    long long hubBefore = getBalance(QR_HUB);

    // Deposit 1,352,000 QU: fee 67,600. shareholder leg = 20% = 13,520; perShare = 20;
    // distributed = 13,520 -> Carol. QPAYHUB leg = 54,080 -> QR_HUB.
    t.depositQU(QR_FUND, pool, 1352000ULL);
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 67600u);
    t.endEpoch();
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 0u);
    EXPECT_EQ(getBalance(QR_CAROL) - carolBefore, 13520LL);
    EXPECT_EQ(getBalance(QR_HUB) - hubBefore, 54080LL);
}

// Token dividend fee: 20% -> QREWARDS shareholders (paid in the token), 80% -> QPAY wallet.
TEST(ContractQRewards, TokenDividendFeeSplit)
{
    ContractTestingQRewards t;

    std::vector<std::pair<m256i, unsigned int>> owners = { { QR_CAROL, NUMBER_OF_COMPUTORS } };
    issueContractShares(QREWARDS_CONTRACT_INDEX, owners, false);

    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);

    t.issueAsset(QR_FUND, QR_DOGE, 5000000LL);
    EXPECT_EQ(t.addDivCurrency(QR_ADMIN, pool, QR_DOGE, QR_FUND), QREWARDS_SUCCESS);

    // Deposit 150,000 QDOGE: fee 7,500. shareholder leg 20% = 1,500; perShare = 2;
    // distributed = 1,352 -> Carol. QPAY leg = 6,148 -> QPAY wallet. 142,500 into pot.
    t.grantMgmtToQRewards(QR_FUND, QR_DOGE, QR_FUND, 150000LL);
    EXPECT_EQ(t.depositAsset(QR_FUND, pool, 1, 150000ULL), QREWARDS_SUCCESS);
    EXPECT_EQ(t.getState()->poolPot(pool, 1), 142500u);
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_FUND, QR_CAROL), 1352u);
    EXPECT_EQ(t.tokenBalanceOf(QR_DOGE, QR_FUND, t.getState()->qpayTokenAddrOf()), 6148u);
}

TEST(ContractQRewards, BatchRegisterAndListAssets)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    EXPECT_EQ(t.registerAssetsBatch(QR_ADMIN, pool, 3, QR_ALICE, 1000000ULL), 3u);
    EXPECT_EQ(t.getState()->poolNumAssets(pool), 3u);
    EXPECT_EQ(t.getAllPoolAssets(pool), 3u);
}

TEST(ContractQRewards, GetPoolsByAdmin)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 20000000000ULL);
    increaseEnergy(QR_ALICE, 20000000000ULL);

    t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.createPool(QR_ALICE, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);

    uint64 first = 0; uint32 total = 0;
    uint32 count = t.getPoolsByAdmin(QR_ADMIN, 0, first, total);
    EXPECT_EQ(total, 2u);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(first, 0u);
}

TEST(ContractQRewards, OperatingFeePauseStopsDistribution)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    // Create pool with NO operating buffer (fee exactly createFee) -> first END_EPOCH
    // pauses it, so its pot is NOT distributed (it carries).
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500

    long long a0 = getBalance(QR_ALICE);
    t.endEpoch(); // operatingBalance 0 < 100k -> paused -> no distribution
    EXPECT_EQ(t.getState()->poolPaused(pool), 1);
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 0LL);   // nothing distributed
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u); // pot carried
}

TEST(ContractQRewards, FundingRouteCreditsPot)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.disableOperatingFee();
    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);

    // QR_FUND routes its plain QU transfers to this pool.
    EXPECT_EQ(t.setFundingRoute(QR_FUND, pool, 0), QREWARDS_SUCCESS);
    // A plain 20,000 QU transfer from QR_FUND -> 5% fee (1,000) -> 19,000 into the pot.
    t.simulateIncomingTransfer(QR_FUND, 20000LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 19000u);
    EXPECT_EQ(t.getState()->pendingDivFeeQUOf(), 1000u);

    // An unrouted sender is ignored (no pot change).
    t.simulateIncomingTransfer(QR_ALICE, 5000LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 19000u);
}

// Streamed mode: END_EPOCH does NOT distribute; END_TICK pays out across the epoch.
// With a large batch the whole cycle finishes in one END_TICK.
TEST(ContractQRewards, StreamedDistributionOneTick)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.enableStreamed(100000u); // streamed, no delay, big batch
    EXPECT_EQ(t.getState()->distributionModeOf(), QREWARDS_DIST_STREAMED);

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0); // Alice weight 75
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500 (fee 500)

    t.beginEpoch();
    long long a0 = getBalance(QR_ALICE);

    // END_EPOCH in streamed mode must NOT pay (distribution is END_TICK's job).
    t.endEpoch();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 0LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 9500u); // still pending

    // One END_TICK completes the cycle: roll pot->distributable, pay Alice the whole 9500.
    t.endTick();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
    EXPECT_EQ(t.getState()->poolDistributable(pool, 0), 0u);
    EXPECT_EQ(t.getState()->cycleActiveOf(), 0);

    // Further END_TICKs in the same epoch are no-ops (one cycle per epoch).
    t.endTick();
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 9500LL);
}

// Streamed mode with batchSize = 1: the cycle spans many END_TICKs but still pays everyone
// exactly once (resumable cursor, no double-pay).
TEST(ContractQRewards, StreamedDistributionAcrossTicks)
{
    ContractTestingQRewards t;
    increaseEnergy(QR_ADMIN, 10000000000ULL);
    increaseEnergy(QR_ALICE, 5000000000ULL);
    increaseEnergy(QR_BOB, 5000000000ULL);
    increaseEnergy(QR_FUND, 5000000000ULL);

    t.enableStreamed(1u); // one work unit per tick

    uint64 pool = t.createPool(QR_ADMIN, 0, QREWARDS_DEFAULT_CREATE_FEE);
    t.issueAsset(QR_ALICE, QR_TOKEN, 50000000LL);
    t.registerAsset(QR_ADMIN, pool, QR_TOKEN, QR_ALICE, 1000000ULL, 10000, 0);
    t.transferAsset(QR_ALICE, QR_TOKEN, QR_ALICE, 25000000LL, QR_BOB); // Alice 25M, Bob 25M -> weight 31 each
    t.depositQU(QR_FUND, pool, 10000ULL); // pot 9500

    t.beginEpoch();
    long long a0 = getBalance(QR_ALICE), b0 = getBalance(QR_BOB);

    // Drive many ticks; the cycle advances one unit each and finishes within a handful.
    for (int i = 0; i < 12; i++) t.endTick();

    EXPECT_EQ(t.getState()->cycleActiveOf(), 0);
    EXPECT_EQ(getBalance(QR_ALICE) - a0, 4750LL); // floor(9500 * 31 / 62)
    EXPECT_EQ(getBalance(QR_BOB) - b0, 4750LL);
    EXPECT_EQ(t.getState()->poolPot(pool, 0), 0u);
    EXPECT_EQ(t.getState()->poolDistributable(pool, 0), 0u);
}
