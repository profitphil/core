using namespace QPI;

// QREWARDS (multi-pool) - open ecosystem loyalty & dividend platform.
//
// Anyone can createPool(): each pool has its own creator/admin, its own internal
// SOULBOUND reward token (internal accounting, not transferable), and its own
// asset registry. Holders of a pool's registered assets earn that pool's token.
//
// Each pool can pay dividends in up to QREWARDS_MAX_DIV_CURRENCIES currencies
// (slot 0 = QU by default; pool admin can add asset currencies, e.g. QDOGE).
// Outside contracts (or anyone) feed a currency into a specific pool via
// depositDividend(poolId, currencyIndex, amount); it is paid pro-rata to that
// pool's DISTRIBUTED balances only. Pools are fully isolated: a pool can only
// ever pay out what was deposited to it.
//
// See docs/qrewards-build-guide.md for the full design rationale.

constexpr uint32 QREWARDS_MAX_POOLS            = 1024;
constexpr uint64 QREWARDS_POSITION_CAPACITY    = 4194304ULL; // 2^22 (poolId,wallet) positions
constexpr uint32 QREWARDS_MAX_ASSETS_PER_POOL  = 64;
constexpr uint32 QREWARDS_MAX_DIV_CURRENCIES   = 4;          // QU + up to 3 assets per pool
constexpr uint32 QREWARDS_MAX_BATCH            = 16;         // assets per registerAssets call
constexpr uint64 QREWARDS_MAX_FUNDERS          = 65536;      // sender->pool routing entries for tagged QU transfers
constexpr uint64 QREWARDS_REGISTRY_SIZE        = (uint64)QREWARDS_MAX_POOLS * QREWARDS_MAX_ASSETS_PER_POOL;
constexpr uint64 QREWARDS_ACC_SCALE            = 1000000ULL;
constexpr uint64 QREWARDS_BPS                  = 10000;
constexpr uint32 QREWARDS_MAX_WEIGHT_BPS       = 1000000;        // cap weight at 100x
constexpr uint64 QREWARDS_DEFAULT_SUPPLY       = 1000000000ULL;  // informational per-pool token supply
constexpr uint64 QREWARDS_DEFAULT_CREATE_FEE   = 1000000ULL;     // anti-spam QU, burned; platform-owner tunable

// return codes
constexpr sint32 QREWARDS_SUCCESS            = 0;
constexpr sint32 QREWARDS_NOT_ADMIN          = 1;
constexpr sint32 QREWARDS_INVALID_PARAM      = 2;
constexpr sint32 QREWARDS_REGISTRY_FULL      = 3;
constexpr sint32 QREWARDS_ASSET_NOT_ISSUED   = 4;
constexpr sint32 QREWARDS_INVALID_INDEX      = 5;
constexpr sint32 QREWARDS_POOL_NOT_FOUND     = 6;
constexpr sint32 QREWARDS_POOL_INACTIVE      = 7;
constexpr sint32 QREWARDS_MAX_POOLS_REACHED  = 8;
constexpr sint32 QREWARDS_INSUFFICIENT_FEE   = 9;
constexpr sint32 QREWARDS_NOT_PLATFORM_OWNER = 10;
constexpr sint32 QREWARDS_CURRENCIES_FULL    = 11;
constexpr sint32 QREWARDS_TRANSFER_FAILED    = 12;

// log types
constexpr uint32 QREWARDS_LOG_SUCCESS       = 0;
constexpr uint32 QREWARDS_LOG_POOL_CREATED  = 1;
constexpr uint32 QREWARDS_LOG_SYNC          = 2;
constexpr uint32 QREWARDS_LOG_DIVIDEND      = 3;
constexpr uint32 QREWARDS_LOG_ASSET_CHANGED = 4;

struct QREWARDS2
{
};

struct QREWARDS : public ContractBase
{
public:
    struct QRewardsLogger
    {
        uint32 _contractIndex;
        uint32 _type;
        sint8 _terminator;
    };

    // A qualifying asset in a pool: holding it earns the pool's reward token.
    struct AssetRule
    {
        uint64 assetName;
        id     issuer;
        uint64 unit;       // quantity of the asset equal to 1 base point
        uint32 weightBps;  // relative value/price multiplier, 10000 = 1.0x
        uint8  kind;       // 0 = fungible token, 1 = contract shares (informational)
        uint8  active;     // 1 = counted, 0 = ignored
    };

    // Spec used by the batch registerAssets.
    struct AssetSpec
    {
        uint64 assetName;
        id     issuer;
        uint64 unit;
        uint32 weightBps;
        uint8  kind;
    };

    // A dividend currency for a pool. assetName==0 && issuer==NULL_ID means QU.
    struct DivCurrency
    {
        uint64 assetName;
        id     issuer;
        uint64 acc;            // accRewardPerTokenScaled for this currency
        uint64 accRemainder;   // carried division remainder (no dust lost)
        uint64 pendingRevenue; // received while totalDistributed == 0
        uint64 lifetime;       // stat
        uint8  active;
    };

    // Per-pool scalars (kept modest so sync/claim read-modify-write stays cheap).
    struct PoolMeta
    {
        id     admin;
        uint64 supply;            // informational pool-token supply
        uint64 totalDistributed;  // undistributed = supply - totalDistributed
        uint64 label;             // short packed name (optional)
        uint32 numAssets;
        uint8  numCurrencies;
        uint8  active;
        Array<DivCurrency, QREWARDS_MAX_DIV_CURRENCIES> currencies;
    };

    // Combined soulbound position: reward balance + per-currency reward debt.
    struct Position
    {
        uint64 profit;
        Array<uint64, QREWARDS_MAX_DIV_CURRENCIES> debt;
    };

    struct KeyProto
    {
        uint64 poolId;
        id     wallet;
    };

    struct StateData
    {
        HashMap<id, Position, QREWARDS_POSITION_CAPACITY> positions; // key = K12(poolId, wallet)
        Array<PoolMeta, QREWARDS_MAX_POOLS> poolMeta;
        Array<AssetRule, QREWARDS_REGISTRY_SIZE> registry;           // pool p asset i at p*MAX_ASSETS+i
        // sourceId -> poolId: a plain QU transfer from sourceId is credited to that pool's QU currency.
        HashMap<id, uint64, QREWARDS_MAX_FUNDERS> fundingRoute;
        uint32 numPools;
        id platformOwner;
        uint64 createPoolFee;
    };

protected:
    /**************************************/
    /************UTIL FUNCTIONS************/
    /**************************************/
    inline static uint64 concentrationMultiplier(uint64 basePoints)
    {
        if (basePoints >= 200) return 20000;
        if (basePoints >= 50)  return 15000;
        if (basePoints >= 10)  return 12500;
        if (basePoints >= 1)   return 10000;
        return 0;
    }

    struct ComputeEntitlement_input { uint64 poolId; id user; };
    struct ComputeEntitlement_output { uint64 profit; };
    struct ComputeEntitlement_locals
    {
        PoolMeta meta;
        uint32 i;
        uint64 base;
        AssetRule rule;
        Asset asset;
        sint64 held;
        uint64 pts;
        uint64 mult;
        uint64 add;
    };
    PRIVATE_FUNCTION_WITH_LOCALS(ComputeEntitlement)
    {
        output.profit = 0;
        locals.meta = state.get().poolMeta.get(input.poolId);
        locals.base = input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numAssets; locals.i++)
        {
            locals.rule = state.get().registry.get(locals.base + locals.i);
            if (!locals.rule.active || locals.rule.unit == 0) continue;
            locals.asset.assetName = locals.rule.assetName;
            locals.asset.issuer = locals.rule.issuer;
            locals.held = qpi.numberOfShares(locals.asset,
                AssetOwnershipSelect::byOwner(input.user),
                AssetPossessionSelect::byPossessor(input.user));
            if (locals.held <= 0) continue;
            locals.pts = div((uint64)locals.held, locals.rule.unit);
            if (locals.pts == 0) continue;
            locals.mult = concentrationMultiplier(locals.pts);
            locals.add = div(locals.pts * locals.mult * (uint64)locals.rule.weightBps,
                             QREWARDS_BPS * QREWARDS_BPS);
            output.profit += locals.add;
        }
    }

    // Pay `amount` of a currency (QU or asset) to `to`.
    struct PayCurrency_input { uint64 assetName; id issuer; id to; uint64 amount; };
    struct PayCurrency_output { bit ok; };
    struct PayCurrency_locals { };
    PRIVATE_PROCEDURE_WITH_LOCALS(PayCurrency)
    {
        output.ok = 0;
        if (input.amount == 0) { output.ok = 1; return; }
        if (input.assetName == 0 && input.issuer == NULL_ID)
        {
            qpi.transfer(input.to, (sint64)input.amount);
            output.ok = 1;
        }
        else
        {
            if (qpi.transferShareOwnershipAndPossession(input.assetName, input.issuer, SELF, SELF,
                    (sint64)input.amount, input.to) >= 0)
            {
                output.ok = 1;
            }
        }
    }

public:
    /**************************************/
    /********PROCEDURES (state-changing)***/
    /**************************************/

    struct createPool_input { uint64 supply; uint64 label; };
    struct createPool_output { sint32 returnCode; uint64 poolId; };
    struct createPool_locals { PoolMeta meta; DivCurrency qu; QRewardsLogger log; };
    PUBLIC_PROCEDURE_WITH_LOCALS(createPool)
    {
        if ((uint64)qpi.invocationReward() < state.get().createPoolFee)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_INSUFFICIENT_FEE;
            return;
        }
        if (state.get().numPools >= QREWARDS_MAX_POOLS)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_MAX_POOLS_REACHED;
            return;
        }
        if (state.get().createPoolFee > 0) qpi.burn((sint64)state.get().createPoolFee);
        if ((uint64)qpi.invocationReward() > state.get().createPoolFee)
            qpi.transfer(qpi.invocator(), qpi.invocationReward() - (sint64)state.get().createPoolFee);

        setMemory(locals.meta, 0);
        locals.meta.admin = qpi.invocator();
        locals.meta.supply = (input.supply == 0) ? QREWARDS_DEFAULT_SUPPLY : input.supply;
        locals.meta.label = input.label;
        locals.meta.active = 1;
        // currency slot 0 = QU by default
        setMemory(locals.qu, 0);
        locals.qu.assetName = 0;
        locals.qu.issuer = NULL_ID;
        locals.qu.active = 1;
        locals.meta.currencies.set(0, locals.qu);
        locals.meta.numCurrencies = 1;

        output.poolId = state.get().numPools;
        state.mut().poolMeta.set(output.poolId, locals.meta);
        state.mut().numPools = state.get().numPools + 1;

        output.returnCode = QREWARDS_SUCCESS;
        locals.log = QRewardsLogger{ CONTRACT_INDEX, QREWARDS_LOG_POOL_CREATED, 0 };
        LOG_INFO(locals.log);
    }

    struct addDividendCurrency_input { uint64 poolId; uint64 assetName; id issuer; };
    struct addDividendCurrency_output { sint32 returnCode; uint32 currencyIndex; };
    struct addDividendCurrency_locals { PoolMeta meta; DivCurrency cur; };
    PUBLIC_PROCEDURE_WITH_LOCALS(addDividendCurrency)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (locals.meta.numCurrencies >= QREWARDS_MAX_DIV_CURRENCIES) { output.returnCode = QREWARDS_CURRENCIES_FULL; return; }
        if (input.assetName == 0 || !qpi.isAssetIssued(input.issuer, input.assetName))
        {
            output.returnCode = QREWARDS_ASSET_NOT_ISSUED;
            return;
        }
        setMemory(locals.cur, 0);
        locals.cur.assetName = input.assetName;
        locals.cur.issuer = input.issuer;
        locals.cur.active = 1;
        locals.meta.currencies.set(locals.meta.numCurrencies, locals.cur);
        output.currencyIndex = locals.meta.numCurrencies;
        locals.meta.numCurrencies++;
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct registerAsset_input { uint64 poolId; uint64 assetName; id issuer; uint64 unit; uint32 weightBps; uint8 kind; };
    struct registerAsset_output { sint32 returnCode; uint32 index; };
    struct registerAsset_locals { PoolMeta meta; AssetRule rule; QRewardsLogger log; };
    PUBLIC_PROCEDURE_WITH_LOCALS(registerAsset)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (!locals.meta.active) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.unit == 0 || input.weightBps == 0 || input.weightBps > QREWARDS_MAX_WEIGHT_BPS)
        { output.returnCode = QREWARDS_INVALID_PARAM; return; }
        if (locals.meta.numAssets >= QREWARDS_MAX_ASSETS_PER_POOL) { output.returnCode = QREWARDS_REGISTRY_FULL; return; }
        if (!qpi.isAssetIssued(input.issuer, input.assetName)) { output.returnCode = QREWARDS_ASSET_NOT_ISSUED; return; }

        locals.rule.assetName = input.assetName;
        locals.rule.issuer = input.issuer;
        locals.rule.unit = input.unit;
        locals.rule.weightBps = input.weightBps;
        locals.rule.kind = input.kind;
        locals.rule.active = 1;
        state.mut().registry.set(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + locals.meta.numAssets, locals.rule);
        output.index = locals.meta.numAssets;
        locals.meta.numAssets++;
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
        locals.log = QRewardsLogger{ CONTRACT_INDEX, QREWARDS_LOG_ASSET_CHANGED, 0 };
        LOG_INFO(locals.log);
    }

    // Batch register up to QREWARDS_MAX_BATCH assets at once. Invalid specs are
    // skipped; output.added reports how many were registered.
    struct registerAssets_input { uint64 poolId; uint32 count; Array<AssetSpec, QREWARDS_MAX_BATCH> specs; };
    struct registerAssets_output { sint32 returnCode; uint32 added; };
    struct registerAssets_locals { PoolMeta meta; AssetSpec spec; AssetRule rule; uint32 i; uint32 n; };
    PUBLIC_PROCEDURE_WITH_LOCALS(registerAssets)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        output.added = 0;
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (!locals.meta.active) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }

        locals.n = (input.count > QREWARDS_MAX_BATCH) ? QREWARDS_MAX_BATCH : input.count;
        for (locals.i = 0; locals.i < locals.n; locals.i++)
        {
            if (locals.meta.numAssets >= QREWARDS_MAX_ASSETS_PER_POOL) break;
            locals.spec = input.specs.get(locals.i);
            if (locals.spec.unit == 0 || locals.spec.weightBps == 0 || locals.spec.weightBps > QREWARDS_MAX_WEIGHT_BPS) continue;
            if (!qpi.isAssetIssued(locals.spec.issuer, locals.spec.assetName)) continue;
            locals.rule.assetName = locals.spec.assetName;
            locals.rule.issuer = locals.spec.issuer;
            locals.rule.unit = locals.spec.unit;
            locals.rule.weightBps = locals.spec.weightBps;
            locals.rule.kind = locals.spec.kind;
            locals.rule.active = 1;
            state.mut().registry.set(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + locals.meta.numAssets, locals.rule);
            locals.meta.numAssets++;
            output.added++;
        }
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct updateAsset_input { uint64 poolId; uint32 index; uint64 unit; uint32 weightBps; uint8 kind; uint8 active; };
    struct updateAsset_output { sint32 returnCode; };
    struct updateAsset_locals { PoolMeta meta; AssetRule rule; };
    PUBLIC_PROCEDURE_WITH_LOCALS(updateAsset)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.index >= locals.meta.numAssets) { output.returnCode = QREWARDS_INVALID_INDEX; return; }
        if (input.unit == 0 || input.weightBps == 0 || input.weightBps > QREWARDS_MAX_WEIGHT_BPS)
        { output.returnCode = QREWARDS_INVALID_PARAM; return; }
        locals.rule = state.get().registry.get(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + input.index);
        locals.rule.unit = input.unit;
        locals.rule.weightBps = input.weightBps;
        locals.rule.kind = input.kind;
        locals.rule.active = (input.active != 0) ? 1 : 0;
        state.mut().registry.set(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + input.index, locals.rule);
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct updateWeight_input { uint64 poolId; uint32 index; uint32 weightBps; };
    struct updateWeight_output { sint32 returnCode; };
    struct updateWeight_locals { PoolMeta meta; AssetRule rule; };
    PUBLIC_PROCEDURE_WITH_LOCALS(updateWeight)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        if (input.index >= locals.meta.numAssets) { output.returnCode = QREWARDS_INVALID_INDEX; return; }
        if (input.weightBps == 0 || input.weightBps > QREWARDS_MAX_WEIGHT_BPS) { output.returnCode = QREWARDS_INVALID_PARAM; return; }
        locals.rule = state.get().registry.get(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + input.index);
        locals.rule.weightBps = input.weightBps;
        state.mut().registry.set(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + input.index, locals.rule);
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct setPoolAdmin_input { uint64 poolId; id newAdmin; };
    struct setPoolAdmin_output { sint32 returnCode; };
    struct setPoolAdmin_locals { PoolMeta meta; };
    PUBLIC_PROCEDURE_WITH_LOCALS(setPoolAdmin)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin) { output.returnCode = QREWARDS_NOT_ADMIN; return; }
        locals.meta.admin = input.newAdmin;
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct syncProfit_input { uint64 poolId; id user; };
    struct syncProfit_output { sint32 returnCode; uint64 profit; };
    struct syncProfit_locals
    {
        PoolMeta meta;
        KeyProto proto;
        id key;
        Position pos;
        DivCurrency cur;
        PayCurrency_input pci;
        PayCurrency_output pco;
        uint32 k;
        uint64 oldBal;
        uint64 newBal;
        uint64 pt;
        uint64 owed;
        ComputeEntitlement_input cei;
        ComputeEntitlement_output ceo;
        QRewardsLogger log;
    };
    // Permissionless: anyone may (re)sync any (pool,user). Idempotent (SET, not ADD).
    PUBLIC_PROCEDURE_WITH_LOCALS(syncProfit)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (!locals.meta.active) { output.returnCode = QREWARDS_POOL_INACTIVE; return; }

        setMemory(locals.proto, 0);
        locals.proto.poolId = input.poolId;
        locals.proto.wallet = input.user;
        locals.key = qpi.K12(locals.proto);

        setMemory(locals.pos, 0);
        state.get().positions.get(locals.key, locals.pos);
        locals.oldBal = locals.pos.profit;

        // 1) settle pending dividends for EVERY currency at the OLD balance.
        for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
        {
            locals.cur = locals.meta.currencies.get(locals.k);
            locals.pt = div(locals.oldBal * locals.cur.acc, QREWARDS_ACC_SCALE);
            if (locals.pt > locals.pos.debt.get(locals.k))
            {
                locals.owed = locals.pt - locals.pos.debt.get(locals.k);
                if (locals.owed > 0)
                {
                    locals.pci.assetName = locals.cur.assetName;
                    locals.pci.issuer = locals.cur.issuer;
                    locals.pci.to = input.user;
                    locals.pci.amount = locals.owed;
                    CALL(PayCurrency, locals.pci, locals.pco);
                }
            }
        }

        // 2) recompute entitlement from live holdings.
        locals.cei.poolId = input.poolId;
        locals.cei.user = input.user;
        CALL(ComputeEntitlement, locals.cei, locals.ceo);
        locals.newBal = locals.ceo.profit;

        // 3) adjust pool total.
        if (locals.newBal >= locals.oldBal) locals.meta.totalDistributed += (locals.newBal - locals.oldBal);
        else locals.meta.totalDistributed -= (locals.oldBal - locals.newBal);
        state.mut().poolMeta.set(input.poolId, locals.meta);

        // 4) store position with fresh per-currency debts (or remove if zero).
        if (locals.newBal > 0)
        {
            locals.pos.profit = locals.newBal;
            for (locals.k = 0; locals.k < QREWARDS_MAX_DIV_CURRENCIES; locals.k++)
            {
                locals.cur = locals.meta.currencies.get(locals.k);
                locals.pos.debt.set(locals.k, div(locals.newBal * locals.cur.acc, QREWARDS_ACC_SCALE));
            }
            state.mut().positions.set(locals.key, locals.pos);
        }
        else
        {
            state.mut().positions.removeByKey(locals.key);
        }

        output.profit = locals.newBal;
        output.returnCode = QREWARDS_SUCCESS;
        locals.log = QRewardsLogger{ CONTRACT_INDEX, QREWARDS_LOG_SYNC, 0 };
        LOG_INFO(locals.log);
    }

    struct claimDividends_input { uint64 poolId; };
    struct claimDividends_output { sint32 returnCode; uint32 currenciesPaid; };
    struct claimDividends_locals
    {
        PoolMeta meta;
        KeyProto proto;
        id key;
        Position pos;
        DivCurrency cur;
        PayCurrency_input pci;
        PayCurrency_output pco;
        uint32 k;
        uint64 pt;
        uint64 owed;
    };
    // Settle pending dividends to the caller for ALL of a pool's currencies.
    PUBLIC_PROCEDURE_WITH_LOCALS(claimDividends)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        output.currenciesPaid = 0;
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);

        setMemory(locals.proto, 0);
        locals.proto.poolId = input.poolId;
        locals.proto.wallet = qpi.invocator();
        locals.key = qpi.K12(locals.proto);

        setMemory(locals.pos, 0);
        if (!state.get().positions.get(locals.key, locals.pos) || locals.pos.profit == 0)
        {
            output.returnCode = QREWARDS_SUCCESS;
            return;
        }
        for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
        {
            locals.cur = locals.meta.currencies.get(locals.k);
            locals.pt = div(locals.pos.profit * locals.cur.acc, QREWARDS_ACC_SCALE);
            if (locals.pt > locals.pos.debt.get(locals.k))
            {
                locals.owed = locals.pt - locals.pos.debt.get(locals.k);
                if (locals.owed > 0)
                {
                    locals.pci.assetName = locals.cur.assetName;
                    locals.pci.issuer = locals.cur.issuer;
                    locals.pci.to = qpi.invocator();
                    locals.pci.amount = locals.owed;
                    CALL(PayCurrency, locals.pci, locals.pco);
                    if (locals.pco.ok) output.currenciesPaid++;
                }
            }
            locals.pos.debt.set(locals.k, locals.pt);
        }
        state.mut().positions.set(locals.key, locals.pos);
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Deposit a dividend into one pool/currency.
    //  - QU currency (slot where assetName==0): QU is taken from the invocation reward.
    //  - asset currency: `amount` of the asset is pulled from the caller into the contract.
    struct depositDividend_input { uint64 poolId; uint32 currencyIndex; uint64 amount; };
    struct depositDividend_output { sint32 returnCode; };
    struct depositDividend_locals
    {
        PoolMeta meta;
        DivCurrency cur;
        uint64 rev;
        uint64 num;
        uint64 inc;
        QRewardsLogger log;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(depositDividend)
    {
        if (input.poolId >= state.get().numPools)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QREWARDS_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (!locals.meta.active || input.currencyIndex >= locals.meta.numCurrencies)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = (!locals.meta.active) ? QREWARDS_POOL_INACTIVE : QREWARDS_INVALID_INDEX;
            return;
        }
        locals.cur = locals.meta.currencies.get(input.currencyIndex);

        if (locals.cur.assetName == 0 && locals.cur.issuer == NULL_ID)
        {
            // QU currency: use the invocation reward.
            locals.rev = (uint64)qpi.invocationReward();
        }
        else
        {
            // Asset currency: refund any QU, pull `amount` of the asset from the caller.
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            if (input.amount == 0) { output.returnCode = QREWARDS_SUCCESS; return; }
            if (qpi.transferShareOwnershipAndPossession(locals.cur.assetName, locals.cur.issuer,
                    qpi.invocator(), qpi.invocator(), (sint64)input.amount, SELF) < 0)
            {
                output.returnCode = QREWARDS_TRANSFER_FAILED;
                return;
            }
            locals.rev = input.amount;
        }

        if (locals.rev == 0) { output.returnCode = QREWARDS_SUCCESS; return; }
        locals.cur.lifetime += locals.rev;
        locals.rev += locals.cur.pendingRevenue;

        if (locals.meta.totalDistributed > 0)
        {
            locals.num = locals.rev * QREWARDS_ACC_SCALE + locals.cur.accRemainder;
            locals.inc = div(locals.num, locals.meta.totalDistributed);
            locals.cur.acc += locals.inc;
            locals.cur.accRemainder = locals.num - locals.inc * locals.meta.totalDistributed;
            locals.cur.pendingRevenue = 0;
        }
        else
        {
            locals.cur.pendingRevenue = locals.rev;
        }
        locals.meta.currencies.set(input.currencyIndex, locals.cur);
        state.mut().poolMeta.set(input.poolId, locals.meta);

        output.returnCode = QREWARDS_SUCCESS;
        locals.log = QRewardsLogger{ CONTRACT_INDEX, QREWARDS_LOG_DIVIDEND, 0 };
        LOG_INFO(locals.log);
    }

    struct setPlatformParams_input { uint64 createPoolFee; };
    struct setPlatformParams_output { sint32 returnCode; };
    PUBLIC_PROCEDURE(setPlatformParams)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (qpi.invocator() != state.get().platformOwner) { output.returnCode = QREWARDS_NOT_PLATFORM_OWNER; return; }
        state.mut().createPoolFee = input.createPoolFee;
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct setPlatformOwner_input { id newOwner; };
    struct setPlatformOwner_output { sint32 returnCode; };
    PUBLIC_PROCEDURE(setPlatformOwner)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (state.get().platformOwner != NULL_ID && qpi.invocator() != state.get().platformOwner)
        { output.returnCode = QREWARDS_NOT_PLATFORM_OWNER; return; }
        state.mut().platformOwner = input.newOwner;
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Register (or clear) a funding route: a plain QU transfer from the caller is
    // auto-credited to poolId's QU currency via POST_INCOMING_TRANSFER. Lets even
    // lower-index contracts fund a pool with a plain qpi.transfer (no procedure call).
    struct setFundingRoute_input { uint64 poolId; bit clear; };
    struct setFundingRoute_output { sint32 returnCode; };
    PUBLIC_PROCEDURE(setFundingRoute)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.clear)
        {
            state.mut().fundingRoute.removeByKey(qpi.invocator());
            output.returnCode = QREWARDS_SUCCESS;
            return;
        }
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        state.mut().fundingRoute.set(qpi.invocator(), input.poolId);
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct TransferShareManagementRights_input { Asset asset; sint64 numberOfShares; uint32 newManagingContractIndex; };
    struct TransferShareManagementRights_output { sint64 transferredNumberOfShares; };
    struct TransferShareManagementRights_locals { sint64 result; };
    PUBLIC_PROCEDURE_WITH_LOCALS(TransferShareManagementRights)
    {
        if (qpi.numberOfPossessedShares(input.asset.assetName, input.asset.issuer,
                qpi.invocator(), qpi.invocator(), SELF_INDEX, SELF_INDEX) < input.numberOfShares)
        {
            output.transferredNumberOfShares = 0;
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            return;
        }
        locals.result = qpi.releaseShares(input.asset, qpi.invocator(), qpi.invocator(),
            input.numberOfShares, input.newManagingContractIndex, input.newManagingContractIndex,
            qpi.invocationReward());
        if (locals.result < 0)
        {
            output.transferredNumberOfShares = 0;
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }
        else
        {
            output.transferredNumberOfShares = input.numberOfShares;
        }
    }

    /**************************************/
    /********FUNCTIONS (read-only)*********/
    /**************************************/

    struct getPosition_input { uint64 poolId; id user; };
    struct getPosition_output
    {
        uint64 profit;
        Array<uint64, QREWARDS_MAX_DIV_CURRENCIES> pending;
        uint8 numCurrencies;
        sint32 returnCode;
    };
    struct getPosition_locals
    {
        PoolMeta meta;
        KeyProto proto;
        id key;
        Position pos;
        DivCurrency cur;
        uint32 k;
        uint64 pt;
    };
    PUBLIC_FUNCTION_WITH_LOCALS(getPosition)
    {
        setMemory(output, 0);
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        setMemory(locals.proto, 0);
        locals.proto.poolId = input.poolId;
        locals.proto.wallet = input.user;
        locals.key = qpi.K12(locals.proto);
        setMemory(locals.pos, 0);
        state.get().positions.get(locals.key, locals.pos);
        output.profit = locals.pos.profit;
        output.numCurrencies = locals.meta.numCurrencies;
        for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
        {
            locals.cur = locals.meta.currencies.get(locals.k);
            locals.pt = div(locals.pos.profit * locals.cur.acc, QREWARDS_ACC_SCALE);
            output.pending.set(locals.k, (locals.pt > locals.pos.debt.get(locals.k)) ? (locals.pt - locals.pos.debt.get(locals.k)) : 0);
        }
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct previewProfit_input { uint64 poolId; id user; };
    struct previewProfit_output { uint64 profit; sint32 returnCode; };
    struct previewProfit_locals { ComputeEntitlement_input cei; ComputeEntitlement_output ceo; };
    PUBLIC_FUNCTION_WITH_LOCALS(previewProfit)
    {
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.cei.poolId = input.poolId;
        locals.cei.user = input.user;
        CALL(ComputeEntitlement, locals.cei, locals.ceo);
        output.profit = locals.ceo.profit;
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct getPool_input { uint64 poolId; };
    struct getPool_output
    {
        id admin;
        uint64 supply;
        uint64 totalDistributed;
        uint64 undistributed;
        uint64 label;
        uint32 numAssets;
        uint8 numCurrencies;
        uint8 active;
        Array<uint64, QREWARDS_MAX_DIV_CURRENCIES> currencyAssetName;
        sint32 returnCode;
    };
    struct getPool_locals { PoolMeta meta; uint32 k; DivCurrency cur; };
    PUBLIC_FUNCTION_WITH_LOCALS(getPool)
    {
        setMemory(output, 0);
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        output.admin = locals.meta.admin;
        output.supply = locals.meta.supply;
        output.totalDistributed = locals.meta.totalDistributed;
        output.undistributed = (locals.meta.supply > locals.meta.totalDistributed)
            ? (locals.meta.supply - locals.meta.totalDistributed) : 0;
        output.label = locals.meta.label;
        output.numAssets = locals.meta.numAssets;
        output.numCurrencies = locals.meta.numCurrencies;
        output.active = locals.meta.active;
        for (locals.k = 0; locals.k < locals.meta.numCurrencies; locals.k++)
        {
            locals.cur = locals.meta.currencies.get(locals.k);
            output.currencyAssetName.set(locals.k, locals.cur.assetName); // 0 = QU
        }
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct getPoolAsset_input { uint64 poolId; uint32 index; };
    struct getPoolAsset_output { uint64 assetName; id issuer; uint64 unit; uint32 weightBps; uint8 kind; uint8 active; sint32 returnCode; };
    struct getPoolAsset_locals { PoolMeta meta; AssetRule rule; };
    PUBLIC_FUNCTION_WITH_LOCALS(getPoolAsset)
    {
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (input.index >= locals.meta.numAssets) { output.returnCode = QREWARDS_INVALID_INDEX; return; }
        locals.rule = state.get().registry.get(input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL + input.index);
        output.assetName = locals.rule.assetName;
        output.issuer = locals.rule.issuer;
        output.unit = locals.rule.unit;
        output.weightBps = locals.rule.weightBps;
        output.kind = locals.rule.kind;
        output.active = locals.rule.active;
        output.returnCode = QREWARDS_SUCCESS;
    }

    // Return every asset in a pool in one call.
    struct getAllPoolAssets_input { uint64 poolId; };
    struct getAllPoolAssets_output
    {
        Array<AssetRule, QREWARDS_MAX_ASSETS_PER_POOL> assets;
        uint32 count;
        sint32 returnCode;
    };
    struct getAllPoolAssets_locals { PoolMeta meta; uint32 i; uint64 base; AssetRule rule; };
    PUBLIC_FUNCTION_WITH_LOCALS(getAllPoolAssets)
    {
        setMemory(output, 0);
        if (input.poolId >= state.get().numPools) { output.returnCode = QREWARDS_POOL_NOT_FOUND; return; }
        locals.meta = state.get().poolMeta.get(input.poolId);
        locals.base = input.poolId * (uint64)QREWARDS_MAX_ASSETS_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numAssets; locals.i++)
        {
            locals.rule = state.get().registry.get(locals.base + locals.i);
            output.assets.set(locals.i, locals.rule);
        }
        output.count = locals.meta.numAssets;
        output.returnCode = QREWARDS_SUCCESS;
    }

    struct getPlatform_input { };
    struct getPlatform_output { id platformOwner; uint64 createPoolFee; uint32 numPools; };
    PUBLIC_FUNCTION(getPlatform)
    {
        output.platformOwner = state.get().platformOwner;
        output.createPoolFee = state.get().createPoolFee;
        output.numPools = state.get().numPools;
    }

    struct getFundingRoute_input { id source; };
    struct getFundingRoute_output { uint64 poolId; bit isSet; };
    struct getFundingRoute_locals { uint64 p; };
    PUBLIC_FUNCTION_WITH_LOCALS(getFundingRoute)
    {
        output.poolId = 0;
        output.isSet = 0;
        if (state.get().fundingRoute.get(input.source, locals.p))
        {
            output.poolId = locals.p;
            output.isSet = 1;
        }
    }

    /**************************************/
    /************REGISTRATION**************/
    /**************************************/
    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
        REGISTER_USER_FUNCTION(getPosition, 1);
        REGISTER_USER_FUNCTION(previewProfit, 2);
        REGISTER_USER_FUNCTION(getPool, 3);
        REGISTER_USER_FUNCTION(getPoolAsset, 4);
        REGISTER_USER_FUNCTION(getPlatform, 5);
        REGISTER_USER_FUNCTION(getAllPoolAssets, 6);
        REGISTER_USER_FUNCTION(getFundingRoute, 7);

        REGISTER_USER_PROCEDURE(createPool, 1);
        REGISTER_USER_PROCEDURE(registerAsset, 2);
        REGISTER_USER_PROCEDURE(updateAsset, 3);
        REGISTER_USER_PROCEDURE(updateWeight, 4);
        REGISTER_USER_PROCEDURE(setPoolAdmin, 5);
        REGISTER_USER_PROCEDURE(syncProfit, 6);
        REGISTER_USER_PROCEDURE(claimDividends, 7);
        REGISTER_USER_PROCEDURE(depositDividend, 8);
        REGISTER_USER_PROCEDURE(setPlatformParams, 9);
        REGISTER_USER_PROCEDURE(setPlatformOwner, 10);
        REGISTER_USER_PROCEDURE(TransferShareManagementRights, 11);
        REGISTER_USER_PROCEDURE(registerAssets, 12);
        REGISTER_USER_PROCEDURE(addDividendCurrency, 13);
        REGISTER_USER_PROCEDURE(setFundingRoute, 14);
    }

    INITIALIZE()
    {
        state.mut().platformOwner = NULL_ID;
        state.mut().createPoolFee = QREWARDS_DEFAULT_CREATE_FEE;
        state.mut().numPools = 0;
    }

    struct END_EPOCH_locals { };
    END_EPOCH_WITH_LOCALS()
    {
        state.mut().positions.cleanupIfNeeded();
        state.mut().fundingRoute.cleanupIfNeeded();
    }

    // Credit plain incoming QU to a routed pool's QU currency (slot 0).
    // Only standard wallet transfers and contract qpi.transfers are routed;
    // procedure/other-contract-invocation transfers are handled by depositDividend,
    // so they are skipped here to avoid double counting.
    struct POST_INCOMING_TRANSFER_locals
    {
        uint64 poolId;
        PoolMeta meta;
        DivCurrency cur;
        uint64 rev;
        uint64 num;
        uint64 inc;
    };
    POST_INCOMING_TRANSFER_WITH_LOCALS()
    {
        if (input.type != TransferType::standardTransaction && input.type != TransferType::qpiTransfer) return;
        if (input.amount <= 0) return;
        if (!state.get().fundingRoute.get(input.sourceId, locals.poolId)) return;
        if (locals.poolId >= state.get().numPools) return;
        locals.meta = state.get().poolMeta.get(locals.poolId);
        if (!locals.meta.active || locals.meta.numCurrencies == 0) return;

        locals.cur = locals.meta.currencies.get(0); // slot 0 is always QU
        locals.rev = (uint64)input.amount;
        locals.cur.lifetime += locals.rev;
        locals.rev += locals.cur.pendingRevenue;
        if (locals.meta.totalDistributed > 0)
        {
            locals.num = locals.rev * QREWARDS_ACC_SCALE + locals.cur.accRemainder;
            locals.inc = div(locals.num, locals.meta.totalDistributed);
            locals.cur.acc += locals.inc;
            locals.cur.accRemainder = locals.num - locals.inc * locals.meta.totalDistributed;
            locals.cur.pendingRevenue = 0;
        }
        else
        {
            locals.cur.pendingRevenue = locals.rev;
        }
        locals.meta.currencies.set(0, locals.cur);
        state.mut().poolMeta.set(locals.poolId, locals.meta);
    }

    PRE_ACQUIRE_SHARES()
    {
        output.allowTransfer = true;
    }
};
