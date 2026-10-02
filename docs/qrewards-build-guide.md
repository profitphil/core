# QREWARDS — Open Multi-Pool Loyalty & Dividend Platform: Build Guide

> Reference implementation: `src/contracts/QRewards.h` (multi-pool / open to anyone).
> Purpose: let anyone create a **pool** with its own internal **soulbound** reward token.
> Holders of a pool's registered ecosystem assets earn that pool's token; outside
> contracts (or anyone) feed QU into a **specific pool** via `depositDividend(poolId)`,
> paid pro-rata to that pool's distributed balances. Pools are fully isolated.

---

## 1. Decisions baked in

| Topic | Decision |
|---|---|
| Multi-tenant | Anyone can `createPool`; each pool has its own admin, token, registry, dividends. |
| Scale | `MAX_POOLS = 1024`, position capacity `2²² = 4,194,304` (poolId,wallet) pairs. |
| State size | ~200–260 MB (well under the 1 GB cap). |
| Dividend currency | **QU**, directed per pool via `depositDividend(poolId)`. |
| Dividend denominator | **Distributed only**, per pool; remainder carry; `pendingRevenue` buffer. |
| Token | **Soulbound** internal accounting (combined `{profit,debt}` value), not transferable. |
| Pool creation | **Open** + small QU fee **burned** (anti-spam, no central beneficiary). Platform owner can retune the fee; takes **no** dividend cut. |
| Keys | A position key = `K12(poolId, wallet)`; `wallet` is a real Qubic public key. |
| Split-resistance | Per-pool reward = proportional base × non-decreasing whole-balance concentration multiplier × admin-set price weight. |

---

## 2. State layout (`src/contracts/QRewards.h`)

```cpp
struct AssetRule { uint64 assetName; id issuer; uint64 unit; uint32 weightBps; uint8 kind; uint8 active; };
struct PoolMeta  { id admin; uint64 supply, totalDistributed, accRewardPerTokenScaled,
                   accRemainder, pendingRevenue, lifetimeRevenue, label; uint32 numAssets; uint8 active; };
struct Position  { uint64 profit; uint64 debt; };     // one combined HashMap value (saves the 32B key duplication)
struct KeyProto  { uint64 poolId; id wallet; };       // zeroed then hashed -> composite key

struct StateData {
    HashMap<id, Position, QREWARDS_POSITION_CAPACITY> positions; // key = K12(poolId, wallet)
    Array<PoolMeta, QREWARDS_MAX_POOLS>  poolMeta;               // hot scalars (small)
    Array<AssetRule, MAX_POOLS*MAX_ASSETS_PER_POOL> registry;   // flat: pool p asset i at p*MAX_ASSETS+i
    uint32 numPools;
    id platformOwner;      // deployer; sets createPoolFee only
    uint64 createPoolFee;  // QU, burned on createPool
};
```

Layout choices that matter:
- **Combined `{profit,debt}` value** — avoids storing the 32-byte key twice (one map, not two).
- **`poolMeta` split from `registry`** — sync/claim read-modify-write only the ~small PoolMeta, never a multi-KB struct.
- **Composite key must be hashed from a zeroed proto** (`setMemory(proto,0)` before setting fields) so padding bytes can't make `K12` nondeterministic. The hashed key is storage-only; payouts use the wallet id we already hold, so the key never needs reversing.

---

## 3. Reward model (per pool, per asset)

```
basePoints = held / unit                       // floor; held < unit => 0
mult       = concentrationMultiplier(basePoints)  // bps, applied to the WHOLE balance
PROFIT_a   = basePoints * mult * weightBps / (10000*10000)
poolProfit = sum over the pool's registered assets
```

- **`unit`** sets accessibility (one base point). No unreachable tier.
- **Concentration multiplier** (whole-balance, non-decreasing → split-proof): `≥1 →1.00×, ≥10 →1.25×, ≥50 →1.50×, ≥200 →2.00×`.
- **`weightBps`** = admin-set relative price (a cheaper asset earns proportionally less).
- Splitting a wallet's holdings never gains, because the multiplier applies to the whole balance (`m` non-decreasing ⇒ concentrated ≥ any split). Note: different *wallets* are different positions on-chain — no on-chain defence against one person using many wallets (needs off-chain identity).

---

## 4. Dividend accounting (per pool, distributed-only)

MasterChef accumulator, `SCALE = 1e6`, denominator = that pool's `totalDistributed`:

```
depositDividend(poolId): rev = invocationReward + meta.pendingRevenue
  if meta.totalDistributed > 0:
      num = rev*SCALE + meta.accRemainder
      inc = num / meta.totalDistributed; meta.acc += inc; meta.accRemainder = num - inc*meta.totalDistributed
      meta.pendingRevenue = 0
  else: meta.pendingRevenue = rev          // nobody to pay yet; QU stays in the contract
```
Per-holder pending = `pos.profit*meta.acc/SCALE - pos.debt`. **Settle before changing a balance**, then `pos.debt = pos.profit*meta.acc/SCALE`.

**Isolation guarantee:** a pool's total claimable = `totalDistributed × acc`, and `acc` only rises from deposits *to that pool*. So even though all QU shares one contract balance, a pool can only ever pay out what was deposited to it — no cross-pool drain.

---

## 5. Procedures & functions

**Procedures** (index): `createPool(1)` · `registerAsset(2)` · `updateAsset(3)` · `updateWeight(4)` · `setPoolAdmin(5)` · `syncProfit(6)` (permissionless) · `claimDividends(7)` · `depositDividend(8)` · `setPlatformParams(9)` · `setPlatformOwner(10)` · `TransferShareManagementRights(11)`.

**Functions** (index): `getPosition(1)` · `previewProfit(2)` · `getPool(3)` · `getPoolAsset(4)` · `getPlatform(5)`.

`syncProfit(poolId, user)` is the heart: settle pending at old balance → recompute entitlement → adjust `poolMeta.totalDistributed` → write/remove the position. Idempotent (SET, not ADD).

---

## 6. Capacity / state size

| Component | Size |
|---|---|
| Position map (2²² × `{id + {profit,debt}}`) | ~193–256 MB (depends on `id` alignment) |
| Registry (1024×64 `AssetRule`) | ~4–6 MB |
| Pool metadata (1024 `PoolMeta`) | ~0.1 MB |
| **Total** | **~200–260 MB** (cap is 1 GB) |

"Positions" = `(pool, wallet)` memberships, not people. One wallet in 3 pools = 3 positions. A wallet that drops to zero holdings has its position reclaimed on the next `syncProfit`, freeing the slot.

---

## 7. Gotchas & security

1. **Settle-before-balance-change** ordering (§4) — non-negotiable.
2. **Zero the KeyProto before K12** — else padding makes keys nondeterministic (fixed in the file).
3. **Multiplier must stay non-decreasing** — the split-proofness invariant.
4. **No O(N) hot paths** — accumulator avoids per-pool payout loops; the only loop is ≤64 registry entries in `computeEntitlement`.
5. **Pool admins are third parties** — each is isolated to its own pool; a pool admin can't touch other pools or the platform.
6. **Platform owner is minimal** — sets the creation fee only; no dividend cut; claim-if-NULL at deploy.
7. **`unit` must be sane** — keep `supply/unit` within safe range so `basePoints × mult × weightBps` can't overflow.
8. **`profit*acc` long-run overflow** — documented; checkpoint if a pool runs enormous lifetime revenue.
9. **Asset must be issued** before `registerAsset` (`isAssetIssued`).

---

## 8. Deployment

1. Register in `contract_core/contract_def.h` (done: index 29, construction epoch 230 = placeholder — set the real one).
2. After construction, the deployer calls `setPlatformOwner` once to claim platform ownership, and `setPlatformParams` to set the creation fee.
3. Anyone then calls `createPool`, `registerAsset`s their ecosystem assets (units/weights per §3), and points their contracts' fee share at `depositDividend(poolId)`.

---

## 9. Legal note

Opening this to third parties makes it a **platform that facilitates others issuing
dividend-bearing tokens** — i.e., helping third parties run what are very likely
**unregistered securities**, plus running a fee-collecting redistributor. That is a
materially larger regulatory surface than personal use (issuer/exchange/transfer-agent,
money-transmission, platform-operator liability). Get securities counsel before opening
it publicly. (Analysis, not legal advice.)
