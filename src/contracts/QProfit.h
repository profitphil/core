using namespace QPI;

// QPROFIT - ecosystem loyalty & dividend contract.
// Holders of qualifying ecosystem assets earn a SOULBOUND internal PROFIT balance
// (internal accounting, not a transferable asset). Ecosystem contracts feed a % fee
// into depositDividend(); QU dividends are paid pro-rata to DISTRIBUTED profit only.
// See docs/qprofit-build-guide.md for the full design rationale.

constexpr uint64 QPROFIT_SUPPLY        = 1000000000ULL; // 1B logical cap on distributable PROFIT
constexpr uint64 QPROFIT_MAX_HOLDERS   = 131072;
constexpr uint32 QPROFIT_MAX_ASSETS    = 64;
constexpr uint64 QPROFIT_ACC_SCALE     = 1000000ULL;    // fixed-point scale for dividend accumulator
constexpr uint64 QPROFIT_BPS           = 10000;         // 10000 = 1.0x
constexpr uint32 QPROFIT_MAX_WEIGHT_BPS = 1000000;      // cap weight at 100x to bound arithmetic

// return codes
constexpr sint32 QPROFIT_SUCCESS          = 0;
constexpr sint32 QPROFIT_NOT_ADMIN        = 1;
constexpr sint32 QPROFIT_INVALID_PARAM    = 2;
constexpr sint32 QPROFIT_REGISTRY_FULL    = 3;
constexpr sint32 QPROFIT_ASSET_NOT_ISSUED = 4;
constexpr sint32 QPROFIT_INVALID_INDEX    = 5;

// log types
constexpr uint32 QPROFIT_LOG_SUCCESS       = 0;
constexpr uint32 QPROFIT_LOG_SYNC          = 1;
constexpr uint32 QPROFIT_LOG_DIVIDEND      = 2;
constexpr uint32 QPROFIT_LOG_ASSET_CHANGED = 3;

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

    // A single qualifying asset and how it maps to PROFIT.
    struct AssetRule
    {
        uint64 assetName;
        id     issuer;
        uint64 unit;       // quantity of the asset equal to 1 base point
        uint32 weightBps;  // relative value/price multiplier, 10000 = 1.0x
        uint8  kind;       // 0 = fungible token, 1 = contract shares (informational)
        uint8  active;     // 1 = counted, 0 = ignored
    };

    struct StateData
    {
        // Soulbound PROFIT balances (distributed) and dividend bookkeeping.
        HashMap<id, uint64, QPROFIT_MAX_HOLDERS> distributedProfit;
        HashMap<id, uint64, QPROFIT_MAX_HOLDERS> rewardDebt;

        Array<AssetRule, QPROFIT_MAX_ASSETS> registry;
        uint32 numAssets;

        uint64 totalDistributed;          // undistributed = QPROFIT_SUPPLY - totalDistributed
        uint64 accRewardPerTokenScaled;   // MasterChef-style accumulator (scaled by ACC_SCALE)
        uint64 accRemainder;              // carried division remainder (no dust lost)
        uint64 pendingRevenue;            // QU received while totalDistributed == 0
        uint64 lifetimeRevenue;           // stat

        id admin;
    };

protected:
    /**************************************/
    /************UTIL FUNCTIONS************/
    /**************************************/
    // Whole-balance concentration multiplier (bps). MUST be non-decreasing:
    // that is the entire split-proofness guarantee.
    inline static uint64 concentrationMultiplier(uint64 basePoints)
    {
        if (basePoints >= 200) return 20000; // 2.00x
        if (basePoints >= 50)  return 15000; // 1.50x
        if (basePoints >= 10)  return 12500; // 1.25x
        if (basePoints >= 1)   return 10000; // 1.00x
        return 0;
    }

    // Compute a user's full PROFIT entitlement from current holdings across the registry.
    struct ComputeEntitlement_input
    {
        id user;
    };
    struct ComputeEntitlement_output
    {
        uint64 profit;
    };
    struct ComputeEntitlement_locals
    {
        uint32 i;
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
        for (locals.i = 0; locals.i < state.get().numAssets; locals.i++)
        {
            locals.rule = state.get().registry.get(locals.i);
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
            // PROFIT_a = pts * mult * weightBps / (BPS * BPS)
            locals.add = div(locals.pts * locals.mult * (uint64)locals.rule.weightBps,
                             QPROFIT_BPS * QPROFIT_BPS);
            output.profit += locals.add;
        }
    }

public:
    /**************************************/
    /********PROCEDURES (state-changing)***/
    /**************************************/

    struct syncProfit_input
    {
        id user;
    };
    struct syncProfit_output
    {
        sint32 returnCode;
        uint64 profit;
    };
    struct syncProfit_locals
    {
        uint64 oldBal;
        uint64 acc;
        uint64 debt;
        uint64 pendingTotal;
        uint64 owed;
        uint64 newBal;
        ComputeEntitlement_input cei;
        ComputeEntitlement_output ceo;
        QProfitLogger log;
    };
    // Permissionless: anyone may (re)sync any user. Idempotent (SET, not ADD).
    PUBLIC_PROCEDURE_WITH_LOCALS(syncProfit)
    {
        // syncProfit takes no fee; return any QU sent.
        if (qpi.invocationReward() > 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }

        locals.acc = state.get().accRewardPerTokenScaled;

        locals.oldBal = 0;
        state.get().distributedProfit.get(input.user, locals.oldBal);
        locals.debt = 0;
        state.get().rewardDebt.get(input.user, locals.debt);

        // 1) settle pending dividends at the OLD balance BEFORE changing anything.
        locals.pendingTotal = div(locals.oldBal * locals.acc, QPROFIT_ACC_SCALE);
        if (locals.pendingTotal > locals.debt)
        {
            locals.owed = locals.pendingTotal - locals.debt;
            if (locals.owed > 0)
            {
                qpi.transfer(input.user, (sint64)locals.owed);
            }
        }

        // 2) recompute entitlement from live holdings.
        locals.cei.user = input.user;
        CALL(ComputeEntitlement, locals.cei, locals.ceo);
        locals.newBal = locals.ceo.profit;

        // 3) adjust totals (branch to avoid unsigned underflow).
        if (locals.newBal >= locals.oldBal)
        {
            state.mut().totalDistributed += (locals.newBal - locals.oldBal);
        }
        else
        {
            state.mut().totalDistributed -= (locals.oldBal - locals.newBal);
        }

        // 4) store balance + fresh reward debt (or remove if now zero).
        if (locals.newBal > 0)
        {
            state.mut().distributedProfit.set(input.user, locals.newBal);
            state.mut().rewardDebt.set(input.user,
                div(locals.newBal * state.get().accRewardPerTokenScaled, QPROFIT_ACC_SCALE));
        }
        else
        {
            state.mut().distributedProfit.removeByKey(input.user);
            state.mut().rewardDebt.removeByKey(input.user);
        }

        output.profit = locals.newBal;
        output.returnCode = QPROFIT_SUCCESS;
        locals.log = QProfitLogger{ CONTRACT_INDEX, QPROFIT_LOG_SYNC, 0 };
        LOG_INFO(locals.log);
    }

    struct claimDividends_input
    {
    };
    struct claimDividends_output
    {
        sint32 returnCode;
        uint64 paid;
    };
    struct claimDividends_locals
    {
        uint64 bal;
        uint64 acc;
        uint64 debt;
        uint64 pendingTotal;
        uint64 owed;
    };
    // Settle pending dividends to the caller without recomputing PROFIT.
    PUBLIC_PROCEDURE_WITH_LOCALS(claimDividends)
    {
        if (qpi.invocationReward() > 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }
        output.paid = 0;
        locals.bal = 0;
        if (!state.get().distributedProfit.get(qpi.invocator(), locals.bal) || locals.bal == 0)
        {
            output.returnCode = QPROFIT_SUCCESS;
            return;
        }
        locals.acc = state.get().accRewardPerTokenScaled;
        locals.debt = 0;
        state.get().rewardDebt.get(qpi.invocator(), locals.debt);
        locals.pendingTotal = div(locals.bal * locals.acc, QPROFIT_ACC_SCALE);
        if (locals.pendingTotal > locals.debt)
        {
            locals.owed = locals.pendingTotal - locals.debt;
            if (locals.owed > 0)
            {
                qpi.transfer(qpi.invocator(), (sint64)locals.owed);
                output.paid = locals.owed;
            }
        }
        state.mut().rewardDebt.set(qpi.invocator(), locals.pendingTotal);
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct depositDividend_input
    {
    };
    struct depositDividend_output
    {
        sint32 returnCode;
    };
    struct depositDividend_locals
    {
        uint64 rev;
        uint64 num;
        uint64 inc;
        QProfitLogger log;
    };
    // Ecosystem contracts (or anyone) send QU here; it becomes dividends for distributed PROFIT.
    PUBLIC_PROCEDURE_WITH_LOCALS(depositDividend)
    {
        locals.rev = (uint64)qpi.invocationReward();
        if (locals.rev == 0)
        {
            output.returnCode = QPROFIT_SUCCESS;
            return;
        }
        state.mut().lifetimeRevenue += locals.rev;
        locals.rev += state.get().pendingRevenue;

        if (state.get().totalDistributed > 0)
        {
            // remainder carry => no dust lost. rev*SCALE fits uint64 for rev up to ~9.2e12 QU.
            locals.num = locals.rev * QPROFIT_ACC_SCALE + state.get().accRemainder;
            locals.inc = div(locals.num, state.get().totalDistributed);
            state.mut().accRewardPerTokenScaled += locals.inc;
            // remainder without mod(): num - inc*totalDistributed
            state.mut().accRemainder = locals.num - locals.inc * state.get().totalDistributed;
            state.mut().pendingRevenue = 0;
        }
        else
        {
            // nobody to pay yet: buffer it (the QU stays in the contract).
            state.mut().pendingRevenue = locals.rev;
        }
        output.returnCode = QPROFIT_SUCCESS;
        locals.log = QProfitLogger{ CONTRACT_INDEX, QPROFIT_LOG_DIVIDEND, 0 };
        LOG_INFO(locals.log);
    }

    struct registerAsset_input
    {
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
        AssetRule rule;
        QProfitLogger log;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(registerAsset)
    {
        if (qpi.invocationReward() > 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }
        if (qpi.invocator() != state.get().admin)
        {
            output.returnCode = QPROFIT_NOT_ADMIN;
            return;
        }
        if (input.unit == 0 || input.weightBps == 0 || input.weightBps > QPROFIT_MAX_WEIGHT_BPS)
        {
            output.returnCode = QPROFIT_INVALID_PARAM;
            return;
        }
        if (state.get().numAssets >= QPROFIT_MAX_ASSETS)
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
        state.mut().registry.set(state.get().numAssets, locals.rule);
        output.index = state.get().numAssets;
        state.mut().numAssets = state.get().numAssets + 1;
        output.returnCode = QPROFIT_SUCCESS;
        locals.log = QProfitLogger{ CONTRACT_INDEX, QPROFIT_LOG_ASSET_CHANGED, 0 };
        LOG_INFO(locals.log);
    }

    struct updateAsset_input
    {
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
        AssetRule rule;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(updateAsset)
    {
        if (qpi.invocationReward() > 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }
        if (qpi.invocator() != state.get().admin)
        {
            output.returnCode = QPROFIT_NOT_ADMIN;
            return;
        }
        if (input.index >= state.get().numAssets)
        {
            output.returnCode = QPROFIT_INVALID_INDEX;
            return;
        }
        if (input.unit == 0 || input.weightBps == 0 || input.weightBps > QPROFIT_MAX_WEIGHT_BPS)
        {
            output.returnCode = QPROFIT_INVALID_PARAM;
            return;
        }
        locals.rule = state.get().registry.get(input.index);
        locals.rule.unit = input.unit;
        locals.rule.weightBps = input.weightBps;
        locals.rule.kind = input.kind;
        locals.rule.active = (input.active != 0) ? 1 : 0;
        state.mut().registry.set(input.index, locals.rule);
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct updateWeight_input
    {
        uint32 index;
        uint32 weightBps;
    };
    struct updateWeight_output
    {
        sint32 returnCode;
    };
    struct updateWeight_locals
    {
        AssetRule rule;
    };
    PUBLIC_PROCEDURE_WITH_LOCALS(updateWeight)
    {
        if (qpi.invocationReward() > 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }
        if (qpi.invocator() != state.get().admin)
        {
            output.returnCode = QPROFIT_NOT_ADMIN;
            return;
        }
        if (input.index >= state.get().numAssets)
        {
            output.returnCode = QPROFIT_INVALID_INDEX;
            return;
        }
        if (input.weightBps == 0 || input.weightBps > QPROFIT_MAX_WEIGHT_BPS)
        {
            output.returnCode = QPROFIT_INVALID_PARAM;
            return;
        }
        locals.rule = state.get().registry.get(input.index);
        locals.rule.weightBps = input.weightBps;
        state.mut().registry.set(input.index, locals.rule);
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct setAdmin_input
    {
        id newAdmin;
    };
    struct setAdmin_output
    {
        sint32 returnCode;
    };
    // Claim-if-NULL bootstrap: the deployer calls this once after construction to claim admin.
    // Afterwards only the current admin can change it.
    PUBLIC_PROCEDURE(setAdmin)
    {
        if (qpi.invocationReward() > 0)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
        }
        if (state.get().admin != NULL_ID && qpi.invocator() != state.get().admin)
        {
            output.returnCode = QPROFIT_NOT_ADMIN;
            return;
        }
        state.mut().admin = input.newAdmin;
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
    // Standard: hand back management of any asset QPROFIT ever ends up managing.
    PUBLIC_PROCEDURE_WITH_LOCALS(TransferShareManagementRights)
    {
        if (qpi.numberOfPossessedShares(input.asset.assetName, input.asset.issuer,
                qpi.invocator(), qpi.invocator(), SELF_INDEX, SELF_INDEX) < input.numberOfShares)
        {
            output.transferredNumberOfShares = 0;
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
            return;
        }
        locals.result = qpi.releaseShares(input.asset, qpi.invocator(), qpi.invocator(),
            input.numberOfShares, input.newManagingContractIndex, input.newManagingContractIndex,
            qpi.invocationReward());
        if (locals.result < 0)
        {
            output.transferredNumberOfShares = 0;
            if (qpi.invocationReward() > 0)
            {
                qpi.transfer(qpi.invocator(), qpi.invocationReward());
            }
        }
        else
        {
            output.transferredNumberOfShares = input.numberOfShares;
        }
    }

    /**************************************/
    /********FUNCTIONS (read-only)*********/
    /**************************************/

    struct getProfit_input
    {
        id user;
    };
    struct getProfit_output
    {
        uint64 profit;
        uint64 pending;
    };
    struct getProfit_locals
    {
        uint64 bal;
        uint64 acc;
        uint64 debt;
        uint64 pt;
    };
    PUBLIC_FUNCTION_WITH_LOCALS(getProfit)
    {
        locals.bal = 0;
        state.get().distributedProfit.get(input.user, locals.bal);
        locals.debt = 0;
        state.get().rewardDebt.get(input.user, locals.debt);
        locals.acc = state.get().accRewardPerTokenScaled;
        locals.pt = div(locals.bal * locals.acc, QPROFIT_ACC_SCALE);
        output.profit = locals.bal;
        output.pending = (locals.pt > locals.debt) ? (locals.pt - locals.debt) : 0;
    }

    struct previewProfit_input
    {
        id user;
    };
    struct previewProfit_output
    {
        uint64 profit;
    };
    struct previewProfit_locals
    {
        ComputeEntitlement_input cei;
        ComputeEntitlement_output ceo;
    };
    PUBLIC_FUNCTION_WITH_LOCALS(previewProfit)
    {
        locals.cei.user = input.user;
        CALL(ComputeEntitlement, locals.cei, locals.ceo);
        output.profit = locals.ceo.profit;
    }

    struct getTotals_input
    {
    };
    struct getTotals_output
    {
        uint64 totalDistributed;
        uint64 undistributed;
        uint64 accRewardPerTokenScaled;
        uint64 pendingRevenue;
        uint64 lifetimeRevenue;
        uint32 numAssets;
    };
    PUBLIC_FUNCTION(getTotals)
    {
        output.totalDistributed = state.get().totalDistributed;
        output.undistributed = QPROFIT_SUPPLY - state.get().totalDistributed;
        output.accRewardPerTokenScaled = state.get().accRewardPerTokenScaled;
        output.pendingRevenue = state.get().pendingRevenue;
        output.lifetimeRevenue = state.get().lifetimeRevenue;
        output.numAssets = state.get().numAssets;
    }

    struct getAsset_input
    {
        uint32 index;
    };
    struct getAsset_output
    {
        uint64 assetName;
        id     issuer;
        uint64 unit;
        uint32 weightBps;
        uint8  kind;
        uint8  active;
        sint32 returnCode;
    };
    struct getAsset_locals
    {
        AssetRule rule;
    };
    PUBLIC_FUNCTION_WITH_LOCALS(getAsset)
    {
        if (input.index >= state.get().numAssets)
        {
            output.returnCode = QPROFIT_INVALID_INDEX;
            return;
        }
        locals.rule = state.get().registry.get(input.index);
        output.assetName = locals.rule.assetName;
        output.issuer = locals.rule.issuer;
        output.unit = locals.rule.unit;
        output.weightBps = locals.rule.weightBps;
        output.kind = locals.rule.kind;
        output.active = locals.rule.active;
        output.returnCode = QPROFIT_SUCCESS;
    }

    struct getAdmin_input
    {
    };
    struct getAdmin_output
    {
        id admin;
    };
    PUBLIC_FUNCTION(getAdmin)
    {
        output.admin = state.get().admin;
    }

    /**************************************/
    /************REGISTRATION**************/
    /**************************************/
    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
        REGISTER_USER_FUNCTION(getProfit, 1);
        REGISTER_USER_FUNCTION(previewProfit, 2);
        REGISTER_USER_FUNCTION(getTotals, 3);
        REGISTER_USER_FUNCTION(getAsset, 4);
        REGISTER_USER_FUNCTION(getAdmin, 5);

        REGISTER_USER_PROCEDURE(syncProfit, 1);
        REGISTER_USER_PROCEDURE(claimDividends, 2);
        REGISTER_USER_PROCEDURE(depositDividend, 3);
        REGISTER_USER_PROCEDURE(registerAsset, 4);
        REGISTER_USER_PROCEDURE(updateAsset, 5);
        REGISTER_USER_PROCEDURE(updateWeight, 6);
        REGISTER_USER_PROCEDURE(setAdmin, 7);
        REGISTER_USER_PROCEDURE(TransferShareManagementRights, 8);
    }

    INITIALIZE()
    {
        // Admin starts NULL: the deployer MUST call setAdmin() once after construction to
        // claim it (claim-if-NULL bootstrap). Do this immediately after deployment.
        state.mut().admin = NULL_ID;
        state.mut().numAssets = 0;
        state.mut().totalDistributed = 0;
        state.mut().accRewardPerTokenScaled = 0;
        state.mut().accRemainder = 0;
        state.mut().pendingRevenue = 0;
        state.mut().lifetimeRevenue = 0;
    }

    struct END_EPOCH_locals
    {
    };
    END_EPOCH_WITH_LOCALS()
    {
        state.mut().distributedProfit.cleanupIfNeeded();
        state.mut().rewardDebt.cleanupIfNeeded();
    }

    PRE_ACQUIRE_SHARES()
    {
        output.allowTransfer = true;
    }
};
