using namespace QPI;

// QPROFIT (multi-pool) - open ecosystem loyalty & dividend platform.
//
// Anyone can createPool(): each pool has its own creator/admin, its own internal
// SOULBOUND reward token (internal accounting, not transferable), and its own
// asset registry. Holders of a pool's registered assets earn that pool's token;
// outside contracts (or anyone) feed QU into a specific pool via depositDividend(poolId),
// which is paid pro-rata to that pool's DISTRIBUTED balances only. Pools are fully
// isolated: a pool can only ever pay out what was deposited to it.
//
// See docs/qprofit-build-guide.md for the full design rationale.

constexpr uint32 QPROFIT_MAX_POOLS           = 1024;
constexpr uint64 QPROFIT_POSITION_CAPACITY   = 4194304ULL; // 2^22 (poolId,wallet) positions
constexpr uint32 QPROFIT_MAX_ASSETS_PER_POOL = 64;
constexpr uint64 QPROFIT_REGISTRY_SIZE       = (uint64)QPROFIT_MAX_POOLS * QPROFIT_MAX_ASSETS_PER_POOL;
constexpr uint64 QPROFIT_ACC_SCALE           = 1000000ULL;
constexpr uint64 QPROFIT_BPS                 = 10000;
constexpr uint32 QPROFIT_MAX_WEIGHT_BPS      = 1000000;        // cap weight at 100x
constexpr uint64 QPROFIT_DEFAULT_SUPPLY      = 1000000000ULL;  // informational per-pool token supply
constexpr uint64 QPROFIT_DEFAULT_CREATE_FEE  = 1000000ULL;     // anti-spam QU, burned; platform-owner tunable

// return codes
constexpr sint32 QPROFIT_SUCCESS          = 0;
constexpr sint32 QPROFIT_NOT_ADMIN        = 1;
constexpr sint32 QPROFIT_INVALID_PARAM    = 2;
constexpr sint32 QPROFIT_REGISTRY_FULL    = 3;
constexpr sint32 QPROFIT_ASSET_NOT_ISSUED = 4;
constexpr sint32 QPROFIT_INVALID_INDEX    = 5;
constexpr sint32 QPROFIT_POOL_NOT_FOUND   = 6;
constexpr sint32 QPROFIT_POOL_INACTIVE    = 7;
constexpr sint32 QPROFIT_MAX_POOLS_REACHED = 8;
constexpr sint32 QPROFIT_INSUFFICIENT_FEE = 9;
constexpr sint32 QPROFIT_NOT_PLATFORM_OWNER = 10;

// log types
constexpr uint32 QPROFIT_LOG_SUCCESS      = 0;
constexpr uint32 QPROFIT_LOG_POOL_CREATED = 1;
constexpr uint32 QPROFIT_LOG_SYNC         = 2;
constexpr uint32 QPROFIT_LOG_DIVIDEND     = 3;
constexpr uint32 QPROFIT_LOG_ASSET_CHANGED = 4;

struct QPROFIT2
{
};

struct QPROFIT : public ContractBase
{
public:
    struct QProfitLogger
    {
        uint32 _contractIndex;
        uint32 _type;
        sint8 _terminator;
    };

    // A single qualifying asset in a pool and how it maps to that pool's token.
    struct AssetRule
    {
        uint64 assetName;
        id     issuer;
        uint64 unit;       // quantity of the asset equal to 1 base point
        uint32 weightBps;  // relative value/price multiplier, 10000 = 1.0x
        uint8  kind;       // 0 = fungible token, 1 = contract shares (informational)
        uint8  active;     // 1 = counted, 0 = ignored
    };

    // Per-pool scalars (kept small so sync/claim read-modify-write is cheap).
    struct PoolMeta
    {
        id     admin;
        uint64 supply;                  // informational pool-token supply
        uint64 totalDistributed;        // undistributed = supply - totalDistributed
        uint64 accRewardPerTokenScaled; // MasterChef-style accumulator (scaled)
        uint64 accRemainder;            // carried division remainder (no dust lost)
        uint64 pendingRevenue;          // QU received while totalDistributed == 0
        uint64 lifetimeRevenue;         // stat
        uint64 label;                   // short packed name (optional)
        uint32 numAssets;
        uint8  active;                  // 1 = pool exists
    };

    // Combined soulbound position value (one HashMap entry per (poolId,wallet)).
    struct Position
    {
        uint64 profit;
        uint64 debt;   // reward-debt snapshot for the accumulator
    };

    // Proto hashed into the composite position key.
    struct KeyProto
    {
        uint64 poolId;
        id     wallet;
    };

    struct StateData
    {
        // Shared soulbound balances, keyed by K12(poolId, wallet).
        HashMap<id, Position, QPROFIT_POSITION_CAPACITY> positions;

        // Per-pool metadata and a flat registry: pool p's asset i lives at p*MAX_ASSETS + i.
        Array<PoolMeta, QPROFIT_MAX_POOLS> poolMeta;
        Array<AssetRule, QPROFIT_REGISTRY_SIZE> registry;
        uint32 numPools;

        id platformOwner;    // deployer; sets createPoolFee only (no dividend cut)
        uint64 createPoolFee; // QU, burned on createPool
    };

protected:
    /**************************************/
    /************UTIL FUNCTIONS************/
    /**************************************/
    // Whole-balance concentration multiplier (bps). MUST be non-decreasing:
    // that is the split-proofness guarantee.
    inline static uint64 concentrationMultiplier(uint64 basePoints)
    {
        if (basePoints >= 200) return 20000; // 2.00x
        if (basePoints >= 50)  return 15000; // 1.50x
        if (basePoints >= 10)  return 12500; // 1.25x
        if (basePoints >= 1)   return 10000; // 1.00x
        return 0;
    }

    struct ComputeEntitlement_input
    {
        uint64 poolId;
        id user;
    };
    struct ComputeEntitlement_output
    {
        uint64 profit;
    };
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
        locals.base = input.poolId * (uint64)QPROFIT_MAX_ASSETS_PER_POOL;
        for (locals.i = 0; locals.i < locals.meta.numAssets; locals.i++)
        {
            locals.rule = state.get().registry.get(locals.base + locals.i);
            if (!locals.rule.active || locals.rule.unit == 0)
            {
                continue;
            }
            locals.asset.assetName = locals.rule.assetName;
            locals.asset.issuer = locals.rule.issuer;
            locals.held = qpi.numberOfShares(locals.asset,
                AssetOwnershipSelect::byOwner(input.user),
                AssetPossessionSelect::byPossessor(input.user));
            if (locals.held <= 0)
            {
                continue;
            }
            locals.pts = div((uint64)locals.held, locals.rule.unit);
            if (locals.pts == 0)
            {
                continue;
            }
            locals.mult = concentrationMultiplier(locals.pts);
            locals.add = div(locals.pts * locals.mult * (uint64)locals.rule.weightBps,
                             QPROFIT_BPS * QPROFIT_BPS);
            output.profit += locals.add;
        }
    }

public:
    /**************************************/
    /********PROCEDURES (state-changing)***/
    /**************************************/

    struct createPool_input
    {
        uint64 supply; // 0 => QPROFIT_DEFAULT_SUPPLY
        uint64 label;  // optional short packed name
    };
    struct createPool_output
    {
        sint32 returnCode;
        uint64 poolId;
    };
    struct createPool_locals
    {
        PoolMeta meta;
        QProfitLogger log;
    };
    // Anyone may create a pool; a small QU fee is burned as anti-spam.
    PUBLIC_PROCEDURE_WITH_LOCALS(createPool)
    {
        if ((uint64)qpi.invocationReward() < state.get().createPoolFee)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QPROFIT_INSUFFICIENT_FEE;
            return;
        }
        if (state.get().numPools >= QPROFIT_MAX_POOLS)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QPROFIT_MAX_POOLS_REACHED;
            return;
        }
        if (state.get().createPoolFee > 0)
        {
            qpi.burn((sint64)state.get().createPoolFee);
        }
        if ((uint64)qpi.invocationReward() > state.get().createPoolFee)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward() - (sint64)state.get().createPoolFee);
        }

        setMemory(locals.meta, 0);
        locals.meta.admin = qpi.invocator();
        locals.meta.supply = (input.supply == 0) ? QPROFIT_DEFAULT_SUPPLY : input.supply;
        locals.meta.label = input.label;
        locals.meta.active = 1;

        output.poolId = state.get().numPools;
        state.mut().poolMeta.set(output.poolId, locals.meta);
        state.mut().numPools = state.get().numPools + 1;

        output.returnCode = QPROFIT_SUCCESS;
        locals.log = QProfitLogger{ CONTRACT_INDEX, QPROFIT_LOG_POOL_CREATED, 0 };
        LOG_INFO(locals.log);
    }

    struct registerAsset_input
    {
        uint64 poolId;
        uint64 assetName;
        id     issuer;
        uint64 unit;
        uint32 weightBps;
        uint8  kind;
    };
    struct registerAsset_output
    {
        sint32 returnCode;
        uint32 index;
    };
    struct registerAsset_locals
    {
        PoolMeta meta;
        AssetRule rule;
        QProfitLogger log;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(registerAsset)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools)
        {
            output.returnCode = QPROFIT_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (!locals.meta.active)
        {
            output.returnCode = QPROFIT_POOL_INACTIVE;
            return;
        }
        if (qpi.invocator() != locals.meta.admin)
        {
            output.returnCode = QPROFIT_NOT_ADMIN;
            return;
        }
        if (input.unit == 0 || input.weightBps == 0 || input.weightBps > QPROFIT_MAX_WEIGHT_BPS)
        {
            output.returnCode = QPROFIT_INVALID_PARAM;
            return;
        }
        if (locals.meta.numAssets >= QPROFIT_MAX_ASSETS_PER_POOL)
        {
            output.returnCode = QPROFIT_REGISTRY_FULL;
            return;
        }
        if (!qpi.isAssetIssued(input.issuer, input.assetName))
        {
            output.returnCode = QPROFIT_ASSET_NOT_ISSUED;
            return;
        }
        locals.rule.assetName = input.assetName;
        locals.rule.issuer = input.issuer;
        locals.rule.unit = input.unit;
        locals.rule.weightBps = input.weightBps;
        locals.rule.kind = input.kind;
        locals.rule.active = 1;
        state.mut().registry.set(input.poolId * (uint64)QPROFIT_MAX_ASSETS_PER_POOL + locals.meta.numAssets, locals.rule);

        output.index = locals.meta.numAssets;
        locals.meta.numAssets++;
        state.mut().poolMeta.set(input.poolId, locals.meta);

        output.returnCode = QPROFIT_SUCCESS;
        locals.log = QProfitLogger{ CONTRACT_INDEX, QPROFIT_LOG_ASSET_CHANGED, 0 };
        LOG_INFO(locals.log);
    }

    struct updateAsset_input
    {
        uint64 poolId;
        uint32 index;
        uint64 unit;
        uint32 weightBps;
        uint8  kind;
        uint8  active;
    };
    struct updateAsset_output
    {
        sint32 returnCode;
    };
    struct updateAsset_locals
    {
        PoolMeta meta;
        AssetRule rule;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(updateAsset)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools)
        {
            output.returnCode = QPROFIT_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin)
        {
            output.returnCode = QPROFIT_NOT_ADMIN;
            return;
        }
        if (input.index >= locals.meta.numAssets)
        {
            output.returnCode = QPROFIT_INVALID_INDEX;
            return;
        }
        if (input.unit == 0 || input.weightBps == 0 || input.weightBps > QPROFIT_MAX_WEIGHT_BPS)
        {
            output.returnCode = QPROFIT_INVALID_PARAM;
            return;
        }
        locals.rule = state.get().registry.get(input.poolId * (uint64)QPROFIT_MAX_ASSETS_PER_POOL + input.index);
        locals.rule.unit = input.unit;
        locals.rule.weightBps = input.weightBps;
        locals.rule.kind = input.kind;
        locals.rule.active = (input.active != 0) ? 1 : 0;
        state.mut().registry.set(input.poolId * (uint64)QPROFIT_MAX_ASSETS_PER_POOL + input.index, locals.rule);
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct updateWeight_input
    {
        uint64 poolId;
        uint32 index;
        uint32 weightBps;
    };
    struct updateWeight_output
    {
        sint32 returnCode;
    };
    struct updateWeight_locals
    {
        PoolMeta meta;
        AssetRule rule;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(updateWeight)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools)
        {
            output.returnCode = QPROFIT_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin)
        {
            output.returnCode = QPROFIT_NOT_ADMIN;
            return;
        }
        if (input.index >= locals.meta.numAssets)
        {
            output.returnCode = QPROFIT_INVALID_INDEX;
            return;
        }
        if (input.weightBps == 0 || input.weightBps > QPROFIT_MAX_WEIGHT_BPS)
        {
            output.returnCode = QPROFIT_INVALID_PARAM;
            return;
        }
        locals.rule = state.get().registry.get(input.poolId * (uint64)QPROFIT_MAX_ASSETS_PER_POOL + input.index);
        locals.rule.weightBps = input.weightBps;
        state.mut().registry.set(input.poolId * (uint64)QPROFIT_MAX_ASSETS_PER_POOL + input.index, locals.rule);
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct setPoolAdmin_input
    {
        uint64 poolId;
        id newAdmin;
    };
    struct setPoolAdmin_output
    {
        sint32 returnCode;
    };
    struct setPoolAdmin_locals
    {
        PoolMeta meta;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(setPoolAdmin)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools)
        {
            output.returnCode = QPROFIT_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (qpi.invocator() != locals.meta.admin)
        {
            output.returnCode = QPROFIT_NOT_ADMIN;
            return;
        }
        locals.meta.admin = input.newAdmin;
        state.mut().poolMeta.set(input.poolId, locals.meta);
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct syncProfit_input
    {
        uint64 poolId;
        id user;
    };
    struct syncProfit_output
    {
        sint32 returnCode;
        uint64 profit;
    };
    struct syncProfit_locals
    {
        PoolMeta meta;
        KeyProto proto;
        id key;
        Position pos;
        uint64 acc;
        uint64 pendingTotal;
        uint64 owed;
        uint64 oldBal;
        uint64 newBal;
        ComputeEntitlement_input cei;
        ComputeEntitlement_output ceo;
        QProfitLogger log;
    };
    // Permissionless: anyone may (re)sync any (pool,user). Idempotent (SET, not ADD).
    PUBLIC_PROCEDURE_WITH_LOCALS(syncProfit)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (input.poolId >= state.get().numPools)
        {
            output.returnCode = QPROFIT_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (!locals.meta.active)
        {
            output.returnCode = QPROFIT_POOL_INACTIVE;
            return;
        }

        setMemory(locals.proto, 0);
        locals.proto.poolId = input.poolId;
        locals.proto.wallet = input.user;
        locals.key = qpi.K12(locals.proto);

        setMemory(locals.pos, 0);
        state.get().positions.get(locals.key, locals.pos);
        locals.oldBal = locals.pos.profit;
        locals.acc = locals.meta.accRewardPerTokenScaled;

        // 1) settle pending dividends at OLD balance before changing anything.
        locals.pendingTotal = div(locals.oldBal * locals.acc, QPROFIT_ACC_SCALE);
        if (locals.pendingTotal > locals.pos.debt)
        {
            locals.owed = locals.pendingTotal - locals.pos.debt;
            if (locals.owed > 0)
            {
                qpi.transfer(input.user, (sint64)locals.owed);
            }
        }

        // 2) recompute entitlement from live holdings.
        locals.cei.poolId = input.poolId;
        locals.cei.user = input.user;
        CALL(ComputeEntitlement, locals.cei, locals.ceo);
        locals.newBal = locals.ceo.profit;

        // 3) adjust pool totals (branch to avoid unsigned underflow).
        if (locals.newBal >= locals.oldBal)
        {
            locals.meta.totalDistributed += (locals.newBal - locals.oldBal);
        }
        else
        {
            locals.meta.totalDistributed -= (locals.oldBal - locals.newBal);
        }
        state.mut().poolMeta.set(input.poolId, locals.meta);

        // 4) store position (or remove if now zero).
        if (locals.newBal > 0)
        {
            locals.pos.profit = locals.newBal;
            locals.pos.debt = div(locals.newBal * locals.meta.accRewardPerTokenScaled, QPROFIT_ACC_SCALE);
            state.mut().positions.set(locals.key, locals.pos);
        }
        else
        {
            state.mut().positions.removeByKey(locals.key);
        }

        output.profit = locals.newBal;
        output.returnCode = QPROFIT_SUCCESS;
        locals.log = QProfitLogger{ CONTRACT_INDEX, QPROFIT_LOG_SYNC, 0 };
        LOG_INFO(locals.log);
    }

    struct claimDividends_input
    {
        uint64 poolId;
    };
    struct claimDividends_output
    {
        sint32 returnCode;
        uint64 paid;
    };
    struct claimDividends_locals
    {
        PoolMeta meta;
        KeyProto proto;
        id key;
        Position pos;
        uint64 acc;
        uint64 pendingTotal;
        uint64 owed;
    };
    // Settle pending dividends to the caller for one pool, without recomputing PROFIT.
    PUBLIC_PROCEDURE_WITH_LOCALS(claimDividends)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        output.paid = 0;
        if (input.poolId >= state.get().numPools)
        {
            output.returnCode = QPROFIT_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);

        setMemory(locals.proto, 0);
        locals.proto.poolId = input.poolId;
        locals.proto.wallet = qpi.invocator();
        locals.key = qpi.K12(locals.proto);

        setMemory(locals.pos, 0);
        if (!state.get().positions.get(locals.key, locals.pos) || locals.pos.profit == 0)
        {
            output.returnCode = QPROFIT_SUCCESS;
            return;
        }
        locals.acc = locals.meta.accRewardPerTokenScaled;
        locals.pendingTotal = div(locals.pos.profit * locals.acc, QPROFIT_ACC_SCALE);
        if (locals.pendingTotal > locals.pos.debt)
        {
            locals.owed = locals.pendingTotal - locals.pos.debt;
            if (locals.owed > 0)
            {
                qpi.transfer(qpi.invocator(), (sint64)locals.owed);
                output.paid = locals.owed;
            }
        }
        locals.pos.debt = locals.pendingTotal;
        state.mut().positions.set(locals.key, locals.pos);
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct depositDividend_input
    {
        uint64 poolId;
    };
    struct depositDividend_output
    {
        sint32 returnCode;
    };
    struct depositDividend_locals
    {
        PoolMeta meta;
        uint64 rev;
        uint64 num;
        uint64 inc;
        QProfitLogger log;
    };
    // Anyone / any contract sends QU here directed at one pool; it becomes that
    // pool's dividends, split pro-rata among that pool's distributed balances.
    PUBLIC_PROCEDURE_WITH_LOCALS(depositDividend)
    {
        if (input.poolId >= state.get().numPools)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QPROFIT_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (!locals.meta.active)
        {
            if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
            output.returnCode = QPROFIT_POOL_INACTIVE;
            return;
        }
        locals.rev = (uint64)qpi.invocationReward();
        if (locals.rev == 0)
        {
            output.returnCode = QPROFIT_SUCCESS;
            return;
        }
        locals.meta.lifetimeRevenue += locals.rev;
        locals.rev += locals.meta.pendingRevenue;

        if (locals.meta.totalDistributed > 0)
        {
            // remainder carry => no dust lost. rev*SCALE fits uint64 for rev up to ~9.2e12 QU.
            locals.num = locals.rev * QPROFIT_ACC_SCALE + locals.meta.accRemainder;
            locals.inc = div(locals.num, locals.meta.totalDistributed);
            locals.meta.accRewardPerTokenScaled += locals.inc;
            locals.meta.accRemainder = locals.num - locals.inc * locals.meta.totalDistributed;
            locals.meta.pendingRevenue = 0;
        }
        else
        {
            locals.meta.pendingRevenue = locals.rev; // nobody to pay yet; QU stays in the contract
        }
        state.mut().poolMeta.set(input.poolId, locals.meta);

        output.returnCode = QPROFIT_SUCCESS;
        locals.log = QProfitLogger{ CONTRACT_INDEX, QPROFIT_LOG_DIVIDEND, 0 };
        LOG_INFO(locals.log);
    }

    struct setPlatformParams_input
    {
        uint64 createPoolFee;
    };
    struct setPlatformParams_output
    {
        sint32 returnCode;
    };
    PUBLIC_PROCEDURE(setPlatformParams)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (qpi.invocator() != state.get().platformOwner)
        {
            output.returnCode = QPROFIT_NOT_PLATFORM_OWNER;
            return;
        }
        state.mut().createPoolFee = input.createPoolFee;
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct setPlatformOwner_input
    {
        id newOwner;
    };
    struct setPlatformOwner_output
    {
        sint32 returnCode;
    };
    // Claim-if-NULL bootstrap: deployer claims platform ownership once after construction.
    PUBLIC_PROCEDURE(setPlatformOwner)
    {
        if (qpi.invocationReward() > 0) qpi.transfer(qpi.invocator(), qpi.invocationReward());
        if (state.get().platformOwner != NULL_ID && qpi.invocator() != state.get().platformOwner)
        {
            output.returnCode = QPROFIT_NOT_PLATFORM_OWNER;
            return;
        }
        state.mut().platformOwner = input.newOwner;
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct TransferShareManagementRights_input
    {
        Asset asset;
        sint64 numberOfShares;
        uint32 newManagingContractIndex;
    };
    struct TransferShareManagementRights_output
    {
        sint64 transferredNumberOfShares;
    };
    struct TransferShareManagementRights_locals
    {
        sint64 result;
    };
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

    struct getPosition_input
    {
        uint64 poolId;
        id user;
    };
    struct getPosition_output
    {
        uint64 profit;
        uint64 pending;
        sint32 returnCode;
    };
    struct getPosition_locals
    {
        PoolMeta meta;
        KeyProto proto;
        id key;
        Position pos;
        uint64 pt;
    };
    PUBLIC_FUNCTION_WITH_LOCALS(getPosition)
    {
        if (input.poolId >= state.get().numPools)
        {
            output.returnCode = QPROFIT_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        setMemory(locals.proto, 0);
        locals.proto.poolId = input.poolId;
        locals.proto.wallet = input.user;
        locals.key = qpi.K12(locals.proto);
        setMemory(locals.pos, 0);
        state.get().positions.get(locals.key, locals.pos);
        locals.pt = div(locals.pos.profit * locals.meta.accRewardPerTokenScaled, QPROFIT_ACC_SCALE);
        output.profit = locals.pos.profit;
        output.pending = (locals.pt > locals.pos.debt) ? (locals.pt - locals.pos.debt) : 0;
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct previewProfit_input
    {
        uint64 poolId;
        id user;
    };
    struct previewProfit_output
    {
        uint64 profit;
        sint32 returnCode;
    };
    struct previewProfit_locals
    {
        ComputeEntitlement_input cei;
        ComputeEntitlement_output ceo;
    };
    PUBLIC_FUNCTION_WITH_LOCALS(previewProfit)
    {
        if (input.poolId >= state.get().numPools)
        {
            output.returnCode = QPROFIT_POOL_NOT_FOUND;
            return;
        }
        locals.cei.poolId = input.poolId;
        locals.cei.user = input.user;
        CALL(ComputeEntitlement, locals.cei, locals.ceo);
        output.profit = locals.ceo.profit;
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct getPool_input
    {
        uint64 poolId;
    };
    struct getPool_output
    {
        id admin;
        uint64 supply;
        uint64 totalDistributed;
        uint64 undistributed;
        uint64 accRewardPerTokenScaled;
        uint64 pendingRevenue;
        uint64 lifetimeRevenue;
        uint64 label;
        uint32 numAssets;
        uint8 active;
        sint32 returnCode;
    };
    struct getPool_locals
    {
        PoolMeta meta;
    };
    PUBLIC_FUNCTION_WITH_LOCALS(getPool)
    {
        if (input.poolId >= state.get().numPools)
        {
            output.returnCode = QPROFIT_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        output.admin = locals.meta.admin;
        output.supply = locals.meta.supply;
        output.totalDistributed = locals.meta.totalDistributed;
        output.undistributed = (locals.meta.supply > locals.meta.totalDistributed)
            ? (locals.meta.supply - locals.meta.totalDistributed) : 0;
        output.accRewardPerTokenScaled = locals.meta.accRewardPerTokenScaled;
        output.pendingRevenue = locals.meta.pendingRevenue;
        output.lifetimeRevenue = locals.meta.lifetimeRevenue;
        output.label = locals.meta.label;
        output.numAssets = locals.meta.numAssets;
        output.active = locals.meta.active;
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct getPoolAsset_input
    {
        uint64 poolId;
        uint32 index;
    };
    struct getPoolAsset_output
    {
        uint64 assetName;
        id     issuer;
        uint64 unit;
        uint32 weightBps;
        uint8  kind;
        uint8  active;
        sint32 returnCode;
    };
    struct getPoolAsset_locals
    {
        PoolMeta meta;
        AssetRule rule;
    };
    PUBLIC_FUNCTION_WITH_LOCALS(getPoolAsset)
    {
        if (input.poolId >= state.get().numPools)
        {
            output.returnCode = QPROFIT_POOL_NOT_FOUND;
            return;
        }
        locals.meta = state.get().poolMeta.get(input.poolId);
        if (input.index >= locals.meta.numAssets)
        {
            output.returnCode = QPROFIT_INVALID_INDEX;
            return;
        }
        locals.rule = state.get().registry.get(input.poolId * (uint64)QPROFIT_MAX_ASSETS_PER_POOL + input.index);
        output.assetName = locals.rule.assetName;
        output.issuer = locals.rule.issuer;
        output.unit = locals.rule.unit;
        output.weightBps = locals.rule.weightBps;
        output.kind = locals.rule.kind;
        output.active = locals.rule.active;
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct getPlatform_input
    {
    };
    struct getPlatform_output
    {
        id platformOwner;
        uint64 createPoolFee;
        uint32 numPools;
    };
    PUBLIC_FUNCTION(getPlatform)
    {
        output.platformOwner = state.get().platformOwner;
        output.createPoolFee = state.get().createPoolFee;
        output.numPools = state.get().numPools;
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
    }

    INITIALIZE()
    {
        // platformOwner starts NULL: deployer MUST call setPlatformOwner() once after
        // construction to claim it. Pools are otherwise fully autonomous.
        state.mut().platformOwner = NULL_ID;
        state.mut().createPoolFee = QPROFIT_DEFAULT_CREATE_FEE;
        state.mut().numPools = 0;
    }

    struct END_EPOCH_locals
    {
    };
    END_EPOCH_WITH_LOCALS()
    {
        state.mut().positions.cleanupIfNeeded();
    }

    PRE_ACQUIRE_SHARES()
    {
        output.allowTransfer = true;
    }
};
