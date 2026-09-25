# LOB Engine — a limit order book matching engine in C++20

A single-instrument limit order book with strict **price–time priority** matching, fed
through a **lock-free SPSC ring buffer** so the matching hot path is free of lock
contention and free of heap allocation under sustained load, and instrumented with
**per-message p50/p99 latency histograms** so every performance claim in this document
can be reproduced and audited from raw bucket dumps.

The project is deliberately built for transparency: the matching semantics are written
down as a spec, an independent naive reference implementation of that spec lives in the
test suite, and a 150,000-operation differential fuzz test demands that the optimized
engine and the readable reference agree on **every event, field for field, and on the
full book state including per-level FIFO order**. The "no allocation on the hot path"
claim is not a comment — it is a test that instruments global `operator new` and fails
if `process()` ever allocates.

```
 feed thread                     matching thread                 event thread
 ───────────                     ───────────────                 ────────────
 synthetic flow ──InboundMsg──▶  SPSC ring ──▶ validate ──▶      SPSC ring ──▶ market data /
 (or your gateway)  (lock-free,    (batch pop)   match            (per-event)   execution reports
      ▲              wait-free)                  price-time                        │
      │                                          rest/cancel                       │
      └────────────── dead-order feedback ring (execution-report loop) ◀───────────┘
```

## Verified results

Numbers were originally measured via `scripts/run_bench.sh` on a single shared cloud vCPU
(Intel Xeon @ 2.80GHz, 4 GB RAM, `--mode inline`, no isolation) — see
[docs/BENCHMARKING.md](docs/BENCHMARKING.md) for that environment's full disclosure. They
were re-measured on 2026-08-09 on a second, unrelated machine (Windows 11, Intel Core
i7-1280P, MinGW-w64 GCC 16.1.0) to check the numbers hold up across different hardware, OS
and compiler and, for the first time, to run the real three-OS-thread `--mode split`
pipeline instead of the single-core fallback. The table below is that split-mode run; an
inline-mode run on the same machine (closer to apples-to-apples with the original
methodology) and the full comparison live in
[docs/BENCHMARKING.md](docs/BENCHMARKING.md).

| profile | offered load | engine throughput | proc p50 | proc p99 | e2e p50 | e2e p99 | e2e p99.9 |
|---|---|---|---|---|---|---|---|
| `max_rate` (closed loop) | unbounded | **1,715,471 msg/s** | 303 ns | 1.50 µs | 38.3 ms † | 48.2 ms † | 49.8 ms † |
| `paced_250k` (open loop) | 250,000 msg/s | 250,000 msg/s sustained | 403 ns | 1.81 µs | 807 ns | 91.1 µs | 540.7 µs |
| `paced_1m` (open loop) | 1,000,000 msg/s | 1,000,000 msg/s sustained | 303 ns | 1.41 µs | 703 ns | 606.2 µs | 1.97 ms |

† `max_rate` is closed-loop by construction: once the ring saturates, a queued message's
e2e time keeps growing for the rest of the run, so (as the harness's own header comment
says) its e2e numbers measure queueing at saturation, not service latency. Read `proc` for
pure engine work; read the paced rows for honest service latency under load.

Each run processes 3–10 million messages of realistic mixed flow (≈56% new orders across
GTC/IOC/FOK/market, ≈34% cancels, ≈10% replaces, ≈37% of accepted volume trading) after a
warmup that is excluded from every statistic. The paced profiles are still the honest
answer to *what latency do you hold at a fixed sustained load*, and both saturate their
offered rate exactly — 250k and 1M msg/s sustained, now confirmed on two independent
machines, backing the "sustained 237k+ msgs/sec" claim. `proc` (pure engine time) matches
or beats the original numbers on this machine; `e2e` tail latency is higher here because
this is a live, unpinned, hybrid P-core/E-core laptop doing its normal background-app
workload at the same time, not a dedicated benchmarking host — not an engine regression.
The full breakdown, including why explicit CPU pinning made tail latency *worse* here, is
in [docs/BENCHMARKING.md](docs/BENCHMARKING.md).

Every profile also writes a JSON summary (`results/*.json`) and a full histogram bucket
dump (`results/*.hist.csv`, every non-empty bucket), so any percentile can be recomputed
independently of this table. (This refresh overwrote the original cloud-container run's
raw files — there was no version control in place to preserve them; that run's numbers
remain recorded in this history and in `docs/BENCHMARKING.md`.)

## What it does

The engine consumes a stream of new-order / cancel / replace messages and emits a
gap-free sequenced stream of execution-report events (`ACCEPTED`, `REJECTED`, `FILL`,
`CANCELED`, `REPLACED`) plus incremental `BOOK_TOP` market-data events whenever the best
bid/offer changes. Supported order flow: limit and market orders; GTC, IOC and
fill-or-kill time-in-force; cancels; and cancel/replace with exchange-realistic priority
semantics (a same-price size-down keeps time priority; a reprice or size-up re-enters the
book and may trade immediately). Fills always execute at the maker's price, best price
first, strict FIFO within a level. Validation covers zero quantities, out-of-band prices,
duplicate ids, unknown cancel/replace targets, and pool exhaustion — capacity limits
convert into `BOOK_FULL` cancels rather than allocations. The full message/event schema
and the matching semantics are specified in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## How the three headline mechanisms work

**Lock contention removed from the hot path.** The feed thread and the matching thread
never share a lock. Handoff happens through `SpscRing`, a bounded wait-free
single-producer/single-consumer queue: the producer writes a 64-byte slot and publishes
it with one release store; the consumer batch-pops with one acquire load per batch.
Producer-owned and consumer-owned indices live on separate cache lines, and each side
keeps a cached copy of the other's index so it only pays a cross-core cache miss when
the ring *looks* full/empty. The memory-ordering argument is written out at the top of
[`include/lob/spsc_ring.hpp`](include/lob/spsc_ring.hpp), and a cross-thread stress test
verifies strict FIFO delivery of two million messages with zero loss or reordering.

**Allocation-free under sustained load.** Everything the hot path touches is pre-sized at
startup: order nodes come from a contiguous `FixedPool` slab, the open-order index is a
flat open-addressing hash table (`FlatMap`) that never rehashes and uses backward-shift
deletion so probe chains stay short over billions of insert/erase cycles, and price
levels are a flat array indexed by tick. `tests/test_no_alloc.cpp` drives 200k mixed
messages through the engine with a counting global `operator new` and requires exactly
zero allocations.

**Latency instrumentation guiding memory layout.** The harness records two HDR-style
log-linear histograms per message (≤1.6% quantization error): `proc` (pure `process()`
time) and `e2e` (producer enqueue → processed, including ring transit and queueing).
The layout decisions in the book — one cache line per order node, 32-byte price levels
packed two per line so level walks prefetch their neighbors, occupancy bitmaps scanned
with `countl_zero`/`countr_zero` so "find next best level after a depletion" is a couple
of word reads instead of a tree walk — are the kind of changes this instrumentation
exists to evaluate, and `--hist-out` dumps the full distributions so before/after
comparisons are done on real percentiles, not vibes.

## Building and running

Requires a C++20 compiler (tested with GCC 13 on Linux and GCC 16/MinGW-w64 on Windows).
With CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure
```

or without CMake:

```bash
scripts/build.sh release     # build/lob_bench, build/lob_replay, build/lob_tests
scripts/build.sh debug       # ASan+UBSan build of the same targets
./build/lob_tests            # 29 tests: units, differential fuzz, no-alloc proof
scripts/run_bench.sh         # the three standard profiles -> results/
```

`lob_bench --help` lists every knob (message count, warmup, paced rate, price band,
capacity, ring sizes, CPU pinning, sampling, JSON/histogram output). On a box with 3+
cores, run `--mode split --pin F,E,V` to dedicate a core to each pipeline stage — but
only on cores you can actually isolate (`isolcpus` or equivalent). On a normal shared
machine, pinning to un-isolated cores can make tail latency *worse*, not better; see
[docs/BENCHMARKING.md](docs/BENCHMARKING.md) §8.

To *watch* the engine think, feed the replay tool a script and it will print every event
plus book snapshots:

```bash
./build/lob_replay examples/sample_session.txt
```

```
> NEW 6 BUY LIMIT 105 15 GTC
  [ 11] ACCEPTED  id=6 BUY px=105 qty=15
  [ 12] FILL      id=3 vs=6 SELL px=104 qty=7 leaves=0 (maker)   <- best price first
  [ 13] FILL      id=6 vs=3 BUY px=104 qty=7 leaves=8 (taker)
  [ 14] FILL      id=1 vs=6 SELL px=105 qty=8 leaves=2 (maker)   <- then oldest at 105
  [ 15] FILL      id=6 vs=1 BUY px=105 qty=8 leaves=0 (taker)
  [ 16] BOOK_TOP  bid=102x8 ask=105x7
```

## Correctness strategy

Three layers, in increasing order of paranoia. First, 27 scripted unit tests pin down
every named behaviour (priority, partial fills, IOC/FOK, market sweeps, cancel/replace
priority rules, best-price advancement across bitmap word boundaries, BBO events,
capacity exhaustion) against exact expected event sequences. Second, the differential
fuzz test in `tests/test_fuzz.cpp` compares the engine against an independent
`std::map`+`std::deque` reference implementation over 150k randomized operations in a
deliberately tight price band, requiring byte-level agreement on every event and
periodic agreement on the entire book including FIFO order — plus a similar differential
stress of `FlatMap` against `std::unordered_map`. Third, structural proofs: the
no-allocation test (passes on every platform this has been built on, including Windows),
and a full-suite run under AddressSanitizer + UBSan (`scripts/build.sh debug`), clean on
Linux/GCC. CI (`.github/workflows/ci.yml`) runs both the release suite and the sanitizer
build on Linux for every change. That sanitizer build currently needs Linux (or a full MSVC/clang-cl setup) to
run at all — a bare MinGW-w64 toolchain doesn't ship `libasan`/`libubsan`; see
`docs/BENCHMARKING.md` §8 for what was actually re-verified on Windows and what wasn't.

## Repository layout

```
include/lob/   header-only engine: common, messages, spsc_ring, pool, flat_map,
               latency, order_book, matching_engine, feed
bench/         measurement harness (threads, pacing, warmup, histograms, JSON)
tools/         lob_replay: deterministic session replay / book explorer
tests/         harness + units + differential fuzz + no-alloc proof
docs/          ARCHITECTURE.md (design + full message/event/semantics spec)
               BENCHMARKING.md (methodology, environment, reproduction)
examples/      sample replay session
scripts/       build.sh, run_bench.sh
results/       committed JSON/txt/histogram-CSV output of the last benchmark
               run (this is what "Verified results" above is generated from)
```

## Scaling and limitations

The engine is single-instrument single-threaded by design — that is what makes it fast
and provable. The intended scale-out is horizontal sharding: one engine instance (one
matching thread, one ring pair) per instrument or instrument group, which is how real
venues partition matching. Within one instrument, throughput headroom above the 250k paced
target is ~7x on this hardware before the closed-loop ceiling. Current limitations,
deliberately out of scope and documented rather than half-built: no self-trade
prevention, no auctions/opening cross, no stop or iceberg order types, no persistence or
replication of engine state, and the price band is fixed at construction (out-of-band
limits are rejected; re-banding is an operational restart). Each of these composes
cleanly with the existing event-sourced design — the gap-free event stream *is* the
journal a persistence layer would consume.

## License

MIT — see [LICENSE](LICENSE).
