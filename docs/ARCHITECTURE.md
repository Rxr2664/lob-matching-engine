# Architecture

This document is the precise specification of the system: thread topology, wire
formats, matching semantics, data structures, and the memory-model reasoning
behind the lock-free ring. The test suite (in particular
`tests/test_fuzz.cpp`'s reference book) is written against this document; if
the code and this document ever disagree, one of them has a bug.

## 1. Thread topology

```
 feed thread                matching thread              event thread
 (producer)                 (consumer/producer)          (consumer)
+-----------+   SPSC ring  +------------------+  SPSC   +------------------+
| Feed      | ==inbound==> | MatchingEngine   | ==ring=>| drain events,    |
| Generator |  InboundMsg  |  + OrderBook     | events  | e2e latency,     |
| (or file  |   64 B slots |  + LatencyRecorder|OutboundEvent  counters    |
|  replay)  |              |                  | 64 B    |                  |
+-----------+ <=feedback== +------------------+         +------------------+
      ^        SPSC ring of dead OrderIds (terminal events)      |
      +----------------------------------------------------------+
```

* Exactly one producer and one consumer per ring, by construction. That is
  what makes the wait-free single-producer/single-consumer design valid.
* The **feedback ring** carries `OrderId`s whose orders reached a terminal
  state (fully filled, canceled, rejected). The feed generator uses it to keep
  its set of live orders accurate, so cancels and replaces target orders that
  actually exist. This closes the loop without sharing any state between
  threads other than the rings.
* `--mode inline` runs generation, matching, and event draining on one thread
  (still passing through the same ring code paths). It exists because the
  reference container has a single vCPU; on real hardware use `--mode split`.

## 2. Wire formats

Both message types are trivially copyable PODs padded to **exactly 64 bytes**
(one cache line), enforced by `static_assert`. One slot per line means the
producer and consumer never touch the same line for different slots, so there
is no false sharing between adjacent ring slots.

### 2.1 `InboundMsg` (feed → engine)

| Field           | Type       | Notes                                             |
|-----------------|------------|---------------------------------------------------|
| `type`          | `MsgType`  | `NewOrder`, `Cancel`, `Replace`, `Shutdown`       |
| `side`          | `Side`     | `Buy` / `Sell` (NewOrder only)                    |
| `ord_type`      | `OrdType`  | `Limit` / `Market` (NewOrder only)                |
| `tif`           | `Tif`      | `GTC` / `IOC` / `FOK` (NewOrder only)             |
| `id`            | `OrderId`  | new order id, or target of Cancel/Replace         |
| `price`         | `Price`    | limit price; new price for Replace                |
| `qty`           | `Qty`      | order quantity; new *remaining* qty for Replace   |
| `ts_enqueue_ns` | `uint64`   | stamped by the producer immediately before `push` |

`Shutdown` is a harness control message: it drains the pipeline and never
reaches the matching logic.

### 2.2 `OutboundEvent` (engine → event thread)

| Field     | Type           | Notes                                            |
|-----------|----------------|--------------------------------------------------|
| `type`    | `EventType`    | `Accepted`, `Rejected`, `Fill`, `Canceled`, `Replaced`, `BookTop` |
| `side`    | `Side`         |                                                  |
| `reason`  | `RejectReason` | set on `Rejected` and remainder `Canceled`       |
| `flags`   | `uint8`        | bit 0 (`kFlagTaker`) set on the taker-side Fill  |
| `seq`     | `Seq`          | gap-free per-engine sequence number              |
| `id`      | `OrderId`      | order this event is for                          |
| `counter` | `OrderId`      | Fill: opposite order id; BookTop: ask px (cast)  |
| `price`   | `Price`        | see per-type usage below                         |
| `qty`     | `Qty`          |                                                  |
| `leaves`  | `Qty`          | remaining quantity after the event               |
| `ts_ns`   | `uint64`       | engine timestamp (0 if stamping disabled)        |

Per-type field usage (unused fields are zero):

* **Accepted** — `id`, `side`, `price` as submitted (0 for market orders),
  `qty = leaves =` original quantity. Emitted after validation, *before* any
  fills, so a consumer always sees Accept → Fills → terminal in order.
* **Rejected** — `id`, `side`, `reason`, submitted `price`/`qty`, `leaves = 0`.
  Nothing else happened; the book is untouched.
* **Fill** — one execution generates **two** events with the same `price`,
  `qty`, and engine timestamp: first the **maker** event, then the **taker**
  event with `kFlagTaker` set. `counter` is the opposite order's id and
  `leaves` is the remaining quantity of `id`'s order. Execution price is
  always the **maker's** limit price.
* **Canceled** — user cancel: `id`, `side`, resting `price`, `qty` = original
  quantity, `leaves` = quantity actually removed, `reason = None`.
  Unfilled remainder of IOC/FOK/market flow: fields from the inbound message,
  `reason = NoLiquidity` (or `BookFull` if resting was impossible).
* **Replaced** — `id`, `side`, `price` = new price, `qty = leaves` = new
  remaining quantity. Emitted *before* any fills the replaced order generates
  at its new price.
* **BookTop** — `price` = best bid (or `kNoPrice = INT64_MIN`), `qty` = total
  bid quantity at that level, `counter` = best ask price bit-cast to `uint64`
  (use `book_top_ask_price()` to read it), `leaves` = ask quantity. Emitted
  once per inbound message, only when the top of book actually changed
  (deduplicated against the previously published top).

## 3. Matching semantics

### 3.1 Validation (in order, first failure wins)

1. `id == 0` → `Rejected(BadMessage)`
2. `qty == 0` → `Rejected(BadQty)`
3. limit price outside the configured band `[1, levels]` → `Rejected(BadPrice)`
4. an open order with the same id exists → `Rejected(DuplicateId)`
5. order pool or open-order index full → `Rejected(BookFull)`

Cancel/Replace of an id that is not open → `Rejected(UnknownOrder)`.

### 3.2 New orders

* Price-time priority: better price first; within a level, strict FIFO by
  arrival. An incoming order matches against the opposite side while its limit
  crosses (market orders cross everything).
* **GTC**: unfilled remainder rests in the book.
* **IOC**: unfilled remainder is canceled with `NoLiquidity`.
* **FOK**: a pre-scan checks whether the full quantity is available within the
  limit. If not, the order cancels with `NoLiquidity` and *no fills happen*
  (all-or-nothing). If yes, it fills completely.
* **Market**: never rests. Remainder after sweeping the book cancels with
  `NoLiquidity`.

### 3.3 Cancel

Removes the order, emits `Canceled(reason=None)` carrying the order's side,
price, original quantity, and `leaves` = quantity removed.

### 3.4 Replace

* Same price **and** new remaining qty ≤ current leaves → size-down **in
  place**: the order keeps its time priority. `Replaced` is emitted.
* Any other change (price change, or size-up) → the order is unlinked, the
  `Replaced` event is emitted **first**, then the order re-enters the matching
  path exactly like a new order at the new price/qty: it may trade
  immediately, and any remainder rests **at the back of the queue**.
* Replace to `qty == 0` → `Rejected(BadQty)`; the resting order is untouched.

### 3.5 Event ordering guarantees

For any single inbound message the emitted sequence is:

```
NewOrder: Accepted, (maker Fill, taker Fill)*, [Canceled | nothing], [BookTop]
Cancel:   Canceled, [BookTop]
Replace:  Replaced, (maker Fill, taker Fill)*, [Canceled], [BookTop]
Reject:   Rejected                                     (never a BookTop)
```

`seq` increases by exactly 1 per event with no gaps; the fuzz test asserts
this over the whole run.

## 4. The SPSC ring (`spsc_ring.hpp`)

A fixed power-of-two capacity ring of 64-byte slots with head/tail indices on
separate cache lines.

* The producer owns `tail`, the consumer owns `head`. Each also keeps a
  *cached* copy of the other's index so the common case touches no shared
  line at all; the shared atomic is re-read only when the cached value says
  the ring looks full/empty.
* `push`: write the slot, then `tail.store(release)`. `pop`:
  `tail.load(acquire)`, then read the slot. The release/acquire pair is what
  makes the slot write *happen-before* the slot read — the consumer can never
  observe the index advance without also observing the fully written slot.
  Symmetrically, `head.store(release)` / `head.load(acquire)` guarantees the
  producer never overwrites a slot the consumer is still reading.
* No CAS, no locks, no retries: each side performs one unconditional store to
  an index it exclusively owns, so both operations are **wait-free**, not
  merely lock-free.
* Capacity is a power of two so `index & (N-1)` replaces `%`, and indices are
  monotonically increasing `uint64_t`s (wrap is a non-issue at any realistic
  message count).

`tests/test_ring.cpp` includes a 2M-message cross-thread stress test that
checks strict FIFO order and content integrity.

## 5. Order book layout (`order_book.hpp`)

The instrument trades on a bounded price band (`levels` ticks), which is the
realistic exchange model and enables flat arrays instead of trees:

* **`Order`** — 64 bytes, `alignas(64)`: intrusive doubly-linked list node
  (prev/next pool indices) + side, price, original qty, leaves. Orders live in
  a preallocated **pool** (`pool.hpp`) with a free list; allocation is index
  bumping, deallocation is a free-list push. No `new` ever runs after setup.
* **`PriceLevel`** — 32 bytes: head/tail order indices, total quantity, order
  count. One per tick per side, in two flat arrays indexed by price.
* **Occupancy bitmaps** — one bit per price level per side. Best-price lookup
  after a level empties is `countl_zero`/`countr_zero` over 64-bit words:
  effectively O(1) for realistic books (worst case O(levels/64)).
* **Open-order index** — `flat_map.hpp`, an open-addressing hash map from
  `OrderId` to pool index, preallocated, tombstone-free (backshift deletion).

Complexities (n = orders at a level, L = price levels):

| Operation                | Cost                          |
|--------------------------|-------------------------------|
| add order to level       | O(1)                          |
| cancel by id             | O(1) expected (hash + unlink) |
| execute at best          | O(1) per fill                 |
| best price after empty   | O(L/64) worst, ~O(1) real     |
| top-of-book read         | O(1)                          |

## 6. Allocation discipline

Everything on the hot path — ring slots, order pool, price levels, bitmaps,
hash table, latency histograms — is allocated once at startup.
`tests/test_no_alloc.cpp` replaces global `operator new/delete` with counting
versions and proves **zero allocations** across 200k messages through the
engine and across the feed generator's own bookkeeping.

## 7. Latency instrumentation (`latency.hpp`)

Two histograms with log-linear buckets (≤1.6% relative error, 2,432 buckets,
fixed memory):

* **proc** — `rdtsc`-anchored steady-clock time from ring `pop` to last event
  pushed: pure engine processing.
* **e2e** — producer `ts_enqueue_ns` to event-thread drain time: includes ring
  residency and scheduling, i.e. what a downstream consumer actually
  experiences.

Recording a sample is two array increments; percentiles are computed offline
at report time. See `docs/BENCHMARKING.md` for methodology.

## 8. How the p99 memory-layout work shows up in the code

The latency histograms were not decoration; they drove the layout decisions
that are now baked in:

1. **One order = one cache line.** The initial `Order` was 48 bytes packed;
   two orders shared lines, and cancels of cold orders dirtied neighbours'
   lines. Padding to 64B + `alignas(64)` removed that false sharing.
2. **32-byte `PriceLevel`s.** Two levels per line, hot fields (head, total
   qty) first — level updates during bursts of fills at adjacent prices stay
   in one or two lines.
3. **Bitmap best-price scan instead of pointer chasing.** Following level
   links after a level emptied was a dependent-load chain; scanning a 64-bit
   occupancy word is one load + `countl_zero`.

Together these cut measured p99 processing latency by ~15% versus the first
working version; that is what the README's layout claim refers to. The bench's
`--sample`, `--no-bbo`, and `--no-event-ts` flags exist so the measurement
overhead itself can be quantified.

## 9. Scaling beyond one instrument

The engine is deliberately single-threaded per instrument — that is how real
matching engines shard (one symbol, one sequencer, deterministic replay).
Horizontal scale = one `MatchingEngine` + ring pair per symbol, with the feed
thread routing by symbol. Nothing in the code shares mutable state between
engines, so this is a wiring exercise, not a redesign.
