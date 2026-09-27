# QPROFIT — Ecosystem Loyalty & Dividend Contract: Build Guide

> Status: design + reference implementation (`src/contracts/QProfit.h`).
> Purpose: reward supporters of the ecosystem. Holders of qualifying ecosystem
> assets earn a **soulbound** internal PROFIT balance; ecosystem contracts feed a
> **% fee** into QPROFIT, which pays **QU dividends** pro‑rata to distributed PROFIT.

---

## 1. Design decisions (locked)

| # | Topic | Decision |
|---|-------|----------|
| 1 | Dividend currency | **QU** (native). |
| 2 | Holder cap | **131,072** (`QPROFIT_MAX_HOLDERS`). O(N) only at settle time — see §7. |
| 3 | Double-counting | `syncProfit` is **idempotent** (SET, not ADD). Sybil handled by the convex whole‑balance multiplier (§5). |
| 4 | Admin | **Single admin ID** (you). Set in `INITIALIZE`; controls the asset registry. |
| 5 | Dividend denominator | **Only distributed PROFIT.** Undistributed is *not* counted and earns nothing. Revenue arriving while `totalDistributed == 0` is buffered in `pendingRevenue`. |

---

## 2. Core model

- **PROFIT is not a tradeable QPI asset.** It is internal accounting (`HashMap<id,uint64>`),
  i.e. **soulbound**. This is what makes *reclaim* possible and keeps PROFIT equal to
  the holder's *current* qualifying support. (Issuing a transferable asset would let
  users move it and break clawback.)
- **PROFIT balance = a pure function of current holdings.** `syncProfit(user)` recomputes
  it from live balances; distribute/reclaim is just the delta.
- **Dividends** accrue to distributed PROFIT via a MasterChef‑style accumulator so there is
  **no O(N) payout loop** and no integer‑rounding loss.
- **1,000,000,000 logical supply** is a ceiling on total distributable PROFIT; with the
  reward sizes below it is effectively non‑binding.

---

## 3. Reading holdings

To read how much of an asset a user holds **regardless of which contract manages it**:

```cpp
Asset a; a.assetName = rule.assetName; a.issuer = rule.issuer;
sint64 held = qpi.numberOfShares(a,
                 AssetOwnershipSelect::byOwner(user),
                 AssetPossessionSelect::byPossessor(user));
```

`byOwner`/`byPossessor` set `anyManagingContract = true`, so this is the user's true
wallet balance of that token or contract‑share. Validate every asset with
`qpi.isAssetIssued(issuer, assetName)` before registering it.

---

## 4. Why the reward is proportional (not a small badge)

Split‑proofness requires **reward‑per‑unit to be non‑decreasing in holdings**. If you also
want **realistic thresholds spanning a wide range** (entry → whale is easily 100×), the
reward must span ≥100× too — i.e. it must be **roughly proportional to holdings**, with a
convex top. Small fixed 1–5 tiers *cannot* be split‑proof over a realistic range. Hence the
model below: a proportional base × a whole‑balance concentration multiplier × a price weight.

---

## 5. Reward formula (per asset, then summed)

```
basePoints = held / unit_asset                     // integer floor; held < unit  => 0 points
mult       = concentrationMultiplier(basePoints)   // bps, applies to the WHOLE balance
PROFIT_a   = basePoints * mult * weightBps_asset / (10000 * 10000)
PROFIT     = sum over registered assets of PROFIT_a
```

**(a) `unit` — accessibility.** Quantity equal to one base point. No unreachable tier — a
whale simply earns proportionally more. Suggested starting units:

| Asset | supply | `unit` (1 base point) |
|-------|--------|------------------------|
| QXMR | 111 b | 1,000,000 |
| QDoge | 21 b | 200,000 |
| Qpay | 736 m | 7,000 |
| Qtreat token | 5,000 | 5 |
| Contract shares (QRaffle / Qtreat / QPAYHUB) | 676 | 1 |

**(b) Concentration multiplier — makes deep holders disproportionately valuable AND kills
splitting.** Applied to the *entire* base‑point balance:

| basePoints | multiplier (bps) |
|-----------:|------------------|
| ≥ 1   | 10000 (1.00×) |
| ≥ 10  | 12500 (1.25×) |
| ≥ 50  | 15000 (1.50×) |
| ≥ 200 | 20000 (2.00×) |

Because the multiplier hits the whole balance, splitting `H` across `k` wallets yields
`(H/unit)·m(H/k) ≤ (H/unit)·m(H)` = concentrated (m non‑decreasing). Concentrating into a
higher band **strictly wins**; equal‑band splits merely tie. Above the top band the reward
is linear (constant top density) — no cap to exploit. **The only split‑proofness invariant to
enforce is: the multiplier is non‑decreasing.**

**(c) `weightBps` — price awareness.** Per‑asset value multiplier, `10000 = 1.0×`, admin‑set to
reflect *relative price*. A share worth 3× another gets 3× weight, so a cheaper share earns
proportionally less at equal count. Maintain via `updateWeight` as prices move. (No reliable
in‑contract spot oracle exists on Qubic; admin‑set weights are the pragmatic v1. An AMM
time‑averaged price could automate it later, at the cost of manipulation risk.)

**Worked example.** Alice: 5 QRaffle shares (weight 3.0×) + 50 m QXMR (unit 1 m → 50 points):
- QRaffle: 5 × 1.00 × 3.0 = **15**
- QXMR: 50 × 1.50 (≥50 band) × 1.0 = **75** → **Total 90**.
Splitting the 50 m into 5×10 m → 5 × (10 × 1.25) = 62.5 < 75. Concentration wins.

---

## 6. Dividend accounting (accumulator, distributed‑only denominator)

State: `accRewardPerTokenScaled` (uint64), `accRemainder` (uint64), `pendingRevenue`
(uint64), plus `rewardDebt : HashMap<id,uint64>`. Scale `QPROFIT_ACC_SCALE = 1e6`.

```
depositDividend(rev = invocationReward):
    rev += pendingRevenue; pendingRevenue = 0
    if totalDistributed > 0:
        num  = rev * QPROFIT_ACC_SCALE + accRemainder      // remainder carry => no dust lost
        accRewardPerTokenScaled += num / totalDistributed
        accRemainder             = num % totalDistributed
    else:
        pendingRevenue += rev                              // nobody to pay yet
```

Per‑holder pending: `bal * acc / SCALE - rewardDebt[user]`.

**Golden rule:** always **settle pending BEFORE changing a holder's PROFIT balance**, then set
`rewardDebt[user] = newBal * acc / SCALE`. Getting this order wrong is the classic
reward‑drain bug.

Overflow notes: keep `SCALE = 1e6` (so `rev * SCALE` fits uint64 for rev up to ~9.2e12 QU per
deposit). `bal * acc` grows over the contract's lifetime; for an extremely long‑lived,
high‑revenue deployment consider a periodic checkpoint/reset (documented limitation).

---

## 7. Procedures & functions

**Procedures**
- `registerAsset(assetName, issuer, unit, weightBps, kind)` — admin. Validates issuance,
  `unit > 0`, registry space; appends an active rule.
- `updateAsset(index, unit, weightBps, kind, active)` — admin. Edit/deactivate a rule.
- `updateWeight(index, weightBps)` — admin. Cheap price re‑tune.
- `syncProfit(user)` — **permissionless** (anyone may poke anyone). Settles pending → recomputes
  entitlement → adjusts `totalDistributed` → writes balance + `rewardDebt`. Idempotent.
- `claimDividends()` — settle pending to caller without recomputing PROFIT.
- `depositDividend()` — called by ecosystem contracts (QU attached) → accumulator (§6).
- `setAdmin(newAdmin)` — admin only (claim‑if‑NULL bootstrap supported).
- `TransferShareManagementRights(...)` — standard; lets QPROFIT hand back management of any
  asset it ever ends up managing.

**Functions (views)**
- `getProfit(user)` → distributed PROFIT + pending QU.
- `previewProfit(user)` → entitlement if synced now.
- `getTotals()` → totalDistributed, undistributed, acc, pendingRevenue.
- `getAsset(index)` / `getRegistrySize()` / `getAdmin()`.

Settle order inside `syncProfit`:
1. `old = distributedProfit[user]` (0 if none); `pending = old*acc/SCALE - rewardDebt[user]`;
   if `pending > 0` `qpi.transfer(user, pending)`.
2. `neu = computeEntitlement(user)`.
3. `totalDistributed += neu - old` (branch on sign to avoid uint underflow).
4. If `neu > 0`: set balance & `rewardDebt = neu*acc/SCALE`; else remove both keys.

---

## 8. State layout

```cpp
struct AssetRule { uint64 assetName; id issuer; uint64 unit; uint32 weightBps; uint8 kind; uint8 active; };

struct StateData {
    HashMap<id, uint64, QPROFIT_MAX_HOLDERS> distributedProfit;
    HashMap<id, uint64, QPROFIT_MAX_HOLDERS> rewardDebt;
    Array<AssetRule, QPROFIT_MAX_ASSETS>     registry;
    uint32 numAssets;
    uint64 totalDistributed;                 // undistributed = SUPPLY - totalDistributed
    uint64 accRewardPerTokenScaled;
    uint64 accRemainder;
    uint64 pendingRevenue;
    uint64 lifetimeRevenue;                  // stat
    id     admin;
};
```

---

## 9. Gotchas & security (put in front of any reviewer)

1. **Settle‑before‑balance‑change** ordering — non‑negotiable (§6).
2. **Multiplier must be non‑decreasing** — the whole split‑proofness guarantee; enforce it if
   bands ever become admin‑tunable.
3. **No O(N) loops on hot paths** — the accumulator avoids per‑epoch payout; the only per‑call
   loop is over the ≤64 registry entries in `computeEntitlement`.
4. **Admin key is powerful** — it defines which assets/weights mint PROFIT. Consider a timelock
   or governance later. Guard against `admin == NULL_ID` misconfig.
5. **Asset validity** — `isAssetIssued` on register; a wrong issuer/name silently reads 0.
6. **Sybil across genuinely different people** is not "double‑counting" — intended.
7. **`bal * acc` long‑run overflow** — documented; checkpoint if needed.
8. **Reentrancy / partial transfer** — `qpi.transfer` returns a status; treat failure sanely
   (don't zero a balance you failed to pay).

---

## 10. Testing checklist

- Entitlement at `unit` boundaries and at each multiplier band (just below / at / above).
- Idempotent `syncProfit` (×3 → same balance, no extra dividends).
- Reclaim path: qualify → drop holdings → `syncProfit` → balance & `totalDistributed` fall.
- Dividend accrual across multiple `depositDividend`s **with** balance changes between them.
- `pendingRevenue` buffer: deposit while `totalDistributed == 0`, then a qualifier appears.
- Remainder carry: many small deposits accumulate with no dust loss.
- Split demonstration: concentrated ≥ any split (document the numbers).
- Registry add / update / deactivate + admin gating (non‑admin rejected).

---

## 11. Wiring into core (deployment)

`QProfit.h` is registered in `src/contract_core/contract_def.h`:
- an include block with `QPROFIT_CONTRACT_INDEX`,
- a `contractDescriptions` row `{"QPROFIT", <constructionEpoch>, 10000, sizeof(QPROFIT::StateData)}`,
- a `REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(QPROFIT)` line.

Set the real **admin public key** in `INITIALIZE` (placeholder is marked in the file), register
your assets post‑construction with `registerAsset`, and point each ecosystem contract's fee
share at `depositDividend`.

---

## 12. Legal note (unchanged from the design discussion)

A token **earned by holding ecosystem assets** that then **pays QU dividends from ecosystem
fees** is a strong **securities** profile (Howey: money in → common enterprise → profit from
others' efforts); soulbound helps optics, not the dividend substance. A fee‑collecting
redistributor can also raise investment‑company questions. Get securities counsel before this
is live. (Analysis, not legal advice.)
