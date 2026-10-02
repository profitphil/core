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
| Pool creation | **Open**. Fee **5,000,000 QU** (protocol-owner tunable), split **70% QREWARDS shareholders / 15% QPAYHUB address / 15% burn**. Any excess seeds the pool's operating balance. |
| Operating fee | **100,000 QU/epoch per pool** (protocol-owner tunable), drawn at `END_EPOCH` from a per-pool operating balance the admin tops up (`depositOperating`); same 70/15/15 split. Underfunded → **paused up to `QREWARDS_MAX_MISSED_EPOCHS` (2) epochs, then deactivated**. |
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

**Procedures** (index): `createPool(1)` · `registerAsset(2)` · `updateAsset(3)` · `updateWeight(4)` · `setPoolAdmin(5)` · `syncProfit(6)` (permissionless) · `claimDividends(7)` · `depositDividend(8)` · `setPlatformParams(9)` · `setPlatformOwner(10)` · `TransferShareManagementRights(11)` · `registerAssets(12)` (batch, ≤16) · `addDividendCurrency(13)` · `setFundingRoute(14)` · `depositOperating(15)` (top up a pool's operating balance) · `setQpayhubAddress(16)` (protocol owner) · `setExcludedAddress(17)` (pool admin).

**Dividend fee:** every dividend deposit is skimmed **5%**; **95% reaches holders**. The 5% is split **80% QPAYHUB / 20% QREWARDS shareholders**. For QU it accrues to `pendingDivFeeQU` and is flushed 80/20 at `END_EPOCH` (20% via `distributeDividends`, 80% transferred to the QPAYHUB address, burned if none set). For a **token** dividend the whole 5% goes to the QPAYHUB address (the 20% shareholder leg needs QU); if no QPAYHUB address is set, no fee is taken on tokens.

**Excluded addresses:** a pool admin calls `setExcludedAddress(poolId, address, excluded)` to exclude/re-include an address (stored as `K12(poolId,address)`). An excluded address's entitlement is forced to **0**, so it neither earns rewards nor receives dividends nor dilutes other holders. A newly-excluded holder with an existing balance is zeroed on its next `syncProfit` (permissionless, so anyone can trigger it). `isExcluded(poolId, address)` is the view.

Fees (create + operating) are split **70% QREWARDS shareholders** (`qpi.distributeDividends`) / **15% to the protocol-owner-set QPAY address** / **15% burned** (if no QPAY address is set, that 15% is burned too). `setPlatformParams(createPoolFee, operatingFee)` and `setQpayhubAddress(addr)` are protocol-owner only.

**How the 15% reaches QPAYHUB's dividend fund:** The latest QPAYHUB (qubic/core **PR #1015**, `src/contracts/QPayhub.h`, **CONTRACT_INDEX 29**) has no deposit procedure — its `POST_INCOMING_TRANSFER` auto-credits any plain QU sent to its address into its `feePool`, which it splits each epoch **10% QPAYHUB shareholders / 1% burn / 89% QPAY token holders** (`QPAYHUB_TOKEN_ASSETNAME = "QPAY"`). QREWARDS's `DistributeFee` already does `qpi.transfer(qpayhubAddress, 15%)`, which lands as a `qpiTransfer` → QPAYHUB `feePool`. So no QPAYHUB call is needed; just set `qpayhubAddress = id(29, 0, 0, 0)` once QPAYHUB is deployed.

**Index:** QPAYHUB occupies **CONTRACT_INDEX 29**, index 30 is another already-deployed contract, so **QREWARDS is wired at index 31**. Indices 29/30 are not part of this fork, so they're held by empty placeholder contracts (`QRewardsReserved29.h`/`QRewardsReserved30.h`) purely to keep the positional contract array consistent — they carry no logic. QPAYHUB also now has an operator role and affiliate registrar (no longer fully admin-free), and its dividend token issuer is still a devnet placeholder to be re-pointed before mainnet. `END_EPOCH` draws the operating fee from every active pool, pausing the underfunded and deactivating after `QREWARDS_MAX_MISSED_EPOCHS`; paused pools reject `syncProfit`/`depositDividend` but still allow `claimDividends`.

**Functions** (index): `getPosition(1)` · `previewProfit(2)` · `getPool(3)` · `getPoolAsset(4)` · `getPlatform(5)` · `getAllPoolAssets(6)` · `getFundingRoute(7)` · `getPoolsByAdmin(8)` (paginated, 256/page) · `getPositions(9)` (one user across ≤64 pools; each row has profit + pending per currency + a `valid` flag) · `isExcluded(10)`.

### Multi-currency dividends

Each pool can pay in up to `QREWARDS_MAX_DIV_CURRENCIES = 4` currencies. Slot 0 is **QU** by default (set at `createPool`); the pool admin adds asset currencies (e.g. QDOGE) with `addDividendCurrency(poolId, assetName, issuer)`. Every currency has its own accumulator, and each position carries a **per-currency reward debt** (`Position.debt[k]`). `claimDividends`/`syncProfit` settle **all** of a pool's currencies.

`depositDividend(poolId, currencyIndex, amount)`:
- **QU currency** (slot where `assetName==0`): QU is taken from the **invocation reward** (`amount` ignored).
- **Asset currency**: the contract **pulls `amount`** of the asset from the caller (`transferShareOwnershipAndPossession(..., caller, caller, amount, SELF)`), so the caller must first have **granted QREWARDS management** of those shares (via `QX.TransferShareManagementRights` to this contract's index), or already hold them under QREWARDS management.

### How outside / future contracts feed a pool

A contract pays a pool by invoking `depositDividend` as a cross-contract call:

```cpp
QREWARDS::depositDividend_input in; in.poolId = POOL; in.currencyIndex = 0; in.amount = 0;
QREWARDS::depositDividend_output out;
INVOKE_OTHER_CONTRACT_PROCEDURE(QREWARDS, depositDividend, in, out, quAmount); // QU via invocationReward
```

**Critical constraint:** Qubic only allows a contract to call another contract **with a lower index**. QREWARDS is index 29, so **only contracts deployed later (index > 29) can call `depositDividend`**. That's fine for *future* contracts (deploy QREWARDS before them). Existing lower-index contracts, and plain user/EOA transactions, feed a pool by having a **user or an off-chain keeper** call `depositDividend` directly (users can call any contract). For asset currencies the caller grants management first (above).

### Tagged QU transfers (no procedure call, works for any index)

A raw QU transfer carries no memo, so tagging is done by a **sender→pool route**. A funder calls `setFundingRoute(poolId, clear=false)` **once**; afterwards any plain QU it sends to the contract is auto-credited to that pool's QU currency (slot 0) by `POST_INCOMING_TRANSFER`. This works for wallets/keepers (`standardTransaction`) **and for any contract via `qpi.transfer`** (`qpiTransfer`) — including **lower-index** contracts that can't `INVOKE_OTHER_CONTRACT_PROCEDURE`. Transfers from senders with no route are left unattributed (effectively donated). `POST_INCOMING_TRANSFER` deliberately ignores `procedureTransaction`/`procedureInvocationByOtherContract` so `depositDividend` is never double-counted. One route per sender; use a dedicated sending address/contract per pool, or `depositDividend` for multi-pool funding.

`syncProfit(poolId, user)` is the heart: settle pending at old balance → recompute entitlement → adjust `poolMeta.totalDistributed` → write/remove the position. Idempotent (SET, not ADD).

---

## 6. Capacity / state size

| Component | Size |
|---|---|
| Position map (2²² × `{id + {profit, debt[4]}}`) | ~385–404 MB (depends on `id` alignment) |
| Registry (1024×64 `AssetRule`) | ~4–6 MB |
| Pool metadata (1024 `PoolMeta`) | ~0.1 MB |
| **Total** | **~400–411 MB** (cap is 1 GB) |

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
