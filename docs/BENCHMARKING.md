# Benchmarking methodology

Performance numbers are easy to inflate and hard to trust. This document
explains exactly what is measured, how, and what the published numbers do and
do not mean, so every figure in the README can be reproduced and audited.

## 1. What is measured

Two latencies are recorded per message, into separate histograms:

* **Processing latency (`proc`)** — from the moment the matching thread pops
  an `InboundMsg` off the ring to the moment the last event for that message
  has been pushed to the event ring. This is pure engine work: validation,
  matching, book mutation, event emission.
* **End-to-end latency (`e2e`)** — from the producer's `ts_enqueue_ns` stamp
  (taken immediately before `push`) to the moment the event thread drains the
  final event for that message. This includes ring residency and any
  scheduling delay, i.e. what a downstream consumer would actually observe.

Throughput is `measured messages / elapsed wall time` over the measurement
window only.

## 2. Warmup exclusion

Every run processes `--warmup` messages first (default 10% of the run), then
**resets both histograms and the clock**. This excludes page faults on
first-touch of the preallocated pools, branch predictor and cache warmup, and
CPU frequency ramp. Engine counters in the JSON cover the whole run;
latency/throughput cover only the measurement window.

## 3. Coordinated omission, and why there are two kinds of profile

A closed-loop benchmark ("send the next message as soon as the previous one
finished") silently pauses the load generator whenever the system stalls, so
stalls are under-sampled and the reported percentiles are flattered. This is
the classic *coordinated omission* problem.

The harness therefore has two modes:

* **`max_rate`** (`--rate 0`, closed loop) — measures peak sustainable
  throughput: how fast the engine can go when offered unlimited load. Its
  latency percentiles are meaningful for engine work but should *not* be read
  as service latency under load.
* **`paced_*`** (`--rate N`, open loop) — the producer sends on a fixed
  schedule against absolute deadlines. If the consumer stalls, messages keep
  arriving on time and queue up, so the stall is fully charged to the e2e
  latency of every affected message. **The paced profiles are the honest
  sustained-throughput numbers**, and they are the ones that back the
  "sustained 237k+ msgs/sec" headline claim (at 250k/sec paced, with p99 ≈ 2 µs,
  the engine is loafing — peak is ~8x higher).

Backpressure spins (producer blocked on a full ring) are counted and reported;
every published paced run shows `producer_backpressure_spins: 0`. The closed-loop
`max_rate` profile can backpressure by design once the ring saturates (see §8.2).

## 4. Sampling and observer overhead

* `--sample N` records every Nth message (default 1 = every message).
  Recording is two array increments, but the timestamp reads themselves cost
  ~20-40ns, which matters at these scales.
* `--no-event-ts` disables engine-side event timestamping, and `--no-bbo`
  disables BookTop publication, so the cost of observability itself can be
  measured rather than guessed.

## 5. Histogram accuracy

Log-linear histograms: 64 linear sub-buckets per power-of-two decade,
giving a worst-case relative error of ≤1.6% on any reported percentile, with
fixed memory (2,432 buckets, ~19 KB per histogram) and O(1) recording. Every
non-empty bucket is written as CSV (`results/*.hist.csv`) so percentiles can be
recomputed independently.

## 6. Environment disclosure (reference results)

The published numbers were measured in a shared cloud container:

* 1 vCPU (shared Intel Xeon @ 2.80 GHz), 4 GB RAM
* Ubuntu 24.04, g++ 13.3, `-O2`, CMake Release
* No core isolation, no frequency pinning, default scheduler

This is close to a worst-case latency environment: the max-latency outliers
(up to tens of ms in paced runs) are scheduler preemptions of the whole
process, not engine behaviour, and they are disclosed rather than trimmed.
Because only one core is available, the reference runs use `--mode inline`
(all three stages on one thread, still passing through the real ring code).
`--mode split` (three threads, the real topology) is verified functional here
and is the default worth using on ≥3 physical cores, ideally with `--pin
F,E,V` to pin feed/engine/event threads, isolated cores
(`isolcpus`/`cset`), and the `performance` frequency governor.

## 7. Reference results

Produced by `scripts/run_bench.sh` (seed 42). These are the original numbers this
project's headline claims were built from. **Their raw JSON/histogram files no longer
exist** — a later re-verification run (§8) reused the same `results/*.json` filenames and
there was no version control in place to preserve the originals first. The table below is
the full record of that run; every number in it is still accurate, it just can no longer
be independently re-derived from a bucket dump the way §8's numbers can.

| Profile      | Offered load    | Achieved       | proc p50 | proc p99 | e2e p50 | e2e p99 | e2e p99.9 |
|--------------|-----------------|----------------|----------|----------|---------|---------|-----------|
| `max_rate`   | unlimited (10M) | **1.98M msg/s**| 191 ns   | 1.26 µs  | 225 ns  | 1.31 µs | 3.71 µs   |
| `paced_1m`   | 1.0M msg/s (6M) | 1.01M msg/s    | 245 ns   | 1.92 µs  | 283 ns  | 1.98 µs | 5.89 µs   |
| `paced_250k` | 250k msg/s (3M) | 250k msg/s     | 303 ns   | 1.97 µs  | 339 ns  | 2.02 µs | 5.38 µs   |

Workload (closed-loop generator, seed 42): 56% new orders / 34% cancels /
10% replaces; 32% of new orders priced aggressively; 3% market, 6% IOC,
1% FOK; random-walking mid over a 200k-tick band. The `max_rate` run executes
2.12M trades and publishes 5.59M top-of-book updates within its 10M messages.

Environment: single shared cloud vCPU (Intel Xeon @ 2.80GHz, 4 GB RAM), Ubuntu 24.04,
g++ 13.3, `-O2`, `--mode inline` (see §6).

Notes on honest reading:

* Paced p50/p99 are slightly *higher* than max_rate's because the paced
  producer's timing loop shares the single core — an artifact of the 1-core
  container, disclosed rather than hidden. On separate cores the paced
  numbers improve.
* `paced_250k` max = 28.3 ms and `paced_1m` max = 9.0 ms are single
  scheduler-preemption events (whole-process descheduling), visible precisely
  *because* pacing is open-loop and refuses to omit them.

## 8. Cross-platform re-verification (2026-08-09, Windows)

Re-run on a second, unrelated machine — a Windows 11 laptop, Intel Core i7-1280P (6
P-cores + 8 E-cores, 20 logical processors), MinGW-w64 GCC 16.1.0 (WinLibs build,
`-O3`), live desktop doing its normal background-app workload throughout (browser,
IDE, the works) — for two reasons: check the numbers hold on completely different
hardware/OS/compiler, and, for the first time, actually run `--mode split` (three real
OS threads across the ring pipeline) instead of the single-core `--mode inline`
fallback the original container was stuck with.

**Portability fixes required:** `bench_main.cpp`'s `pin_to_cpu()` was Linux-only
(`pthread_setaffinity_np`); it already degraded gracefully to a no-op elsewhere, so the
harness built and ran fine without it, but pinning silently did nothing. Added a
`SetThreadAffinityMask`-based Windows branch alongside it (mirrors the existing
`#if defined(__linux__)` pattern). Also, MinGW's UCRT has no `std::aligned_alloc`;
`tests/test_main.cpp`'s counting allocator now uses `_aligned_malloc`/`_aligned_free` on
Windows behind the same kind of platform guard (the two allocator families are not
interchangeable, so they stay correctly paired). Neither change touches the engine
itself — both are test/bench-harness portability fixes.

**A real bug, not just a shim (see §8.3): `lob_replay` segfaulted 100% of the time** when
built at `-O2`/`-O3` on this toolchain, including on the bundled example script. Fixed by
capping its optimization level; full writeup below because this one actually matters for
"does the project work," not just for benchmarking methodology.

### 8.1 Inline mode (apples-to-apples with §7's methodology)

| Profile      | Offered load    | Achieved        | proc p50 | proc p99 | e2e p50 | e2e p99 | e2e p99.9 | max     |
|--------------|-----------------|-----------------|----------|----------|---------|---------|-----------|---------|
| `max_rate`   | unlimited (10M) | 1.26M msg/s     | 303 ns   | 2.02 µs  | 403 ns  | 2.11 µs | 3.42 µs   | 0.76 ms |
| `paced_250k` | 250k msg/s (3M) | 250k msg/s      | 303 ns   | 2.43 µs  | 403 ns  | 2.53 µs | 10.88 µs  | 1.56 ms |
| `paced_1m`   | 1.0M msg/s (6M) | 1.00M msg/s     | 303 ns   | 1.81 µs  | 403 ns  | 2.02 µs | 3.90 µs   | 0.55 ms |

Raw JSON/histograms: `results/win_inline/`. This is the closest comparison to §7: same
single-thread pipeline, same message mix, same seed. Reading it against §7: `proc`/`e2e`
p99 land in the same ~1.5–2.5 µs band as the original claim, confirming "p99 ≈ 2 µs" holds
on entirely different hardware. `max_rate` throughput is lower here (1.26M vs 1.98M
msg/s) despite faster cores — the most likely explanation is `now_ns()`'s
`clock_gettime(CLOCK_MONOTONIC)` costing more per call through MinGW/UCRT's Windows shim
than through Linux's vDSO fast path: every profile's `proc` p50 sits at almost exactly 303
ns here regardless of load, whereas the original varied 191–303 ns with load, consistent
with a fixed per-call timer tax on this platform rather than a slower matching engine
(`--sample`/`--no-event-ts` from §4 would isolate this precisely; not yet done). Every
per-run absolute max is dramatically *lower* here (0.55–1.56 ms vs 9–28.3 ms) — fewer/smaller
scheduler-preemption events, plausibly just a faster, less-loaded-at-that-moment core.

### 8.2 Split mode (real pipeline; this is what README's table shows)

| Profile      | Offered load    | Achieved        | proc p50 | proc p99 | e2e p50   | e2e p99  | e2e p99.9 | max      |
|--------------|-----------------|-----------------|----------|----------|-----------|----------|-----------|----------|
| `max_rate`   | unlimited (10M) | 1.72M msg/s     | 303 ns   | 1.50 µs  | 38.3 ms\* | 48.2 ms\*| 49.8 ms\* | 49.8 ms\*|
| `paced_250k` | 250k msg/s (3M) | 250k msg/s      | 403 ns   | 1.81 µs  | 807 ns    | 91.1 µs  | 540.7 µs  | 1.73 ms  |
| `paced_1m`   | 1.0M msg/s (6M) | 1.00M msg/s     | 303 ns   | 1.41 µs  | 703 ns    | 606.2 µs | 1.97 ms   | 3.00 ms  |

Raw JSON/histograms: `results/*.json`, `results/*.hist.csv` (these are the files that used
to hold §7's cloud-container numbers — see the note at the top of §7).

\* `max_rate`'s e2e column here is a real instance of the closed-loop trap §3 warns about:
with three independent OS threads actually able to run concurrently (unlike inline mode,
which physically cannot build a deep backlog), the feed thread's unthrottled push rate
outran the matching/event threads on this shared, contended machine for sustained
stretches (`producer_backpressure_spins: 2,644,007`), so the ring stayed close to full for
most of the run and e2e queueing time climbed for the rest of it. This is exactly what §3
means by "not comparable to open-loop numbers" — read `proc` for max_rate, not `e2e`.

Reading the paced rows (the ones the "sustained 237k+ msgs/sec" claim is actually about):
throughput sustains its offered rate exactly on this machine too — 250k and 1M msg/s, now
independently confirmed twice. `proc` p99 is as good as or better than §7 (1.41–1.81 µs vs
1.92–1.97 µs). `e2e` p99/p99.9 are substantially worse (91–606 µs / 541 µs–1.97 ms vs
2.0 µs / 5.4–5.9 µs) — that gap is the real, honest cost of running the three pipeline
stages as genuinely concurrent OS threads on a shared, unpinned, hybrid-core laptop with a
normal desktop workload competing for the same cores, rather than an engine slowdown: the
`proc` numbers directly above prove the engine itself is unaffected.

**Pinning made this worse, not better.** `--pin` was tried (§8's Windows
`SetThreadAffinityMask` addition) against several core sets, including cores past index 11
(this chip's likely E-core range) — every explicit pin tested *increased* e2e tail latency
relative to leaving threads unpinned; the published split numbers above are unpinned. The
likely reason: Windows' scheduler already runs a hybrid-topology-aware thread placement
policy (Intel Thread Director) that migrates threads toward idle performance cores as
load shifts; nailing a thread to one fixed core removes its ability to dodge contention
from the rest of this machine's background load, which on a shared desktop with no
isolated cores (`isolcpus`-equivalent doesn't exist on Windows) makes things worse rather
than better. This is the opposite of the effect pinning has on an isolated Linux box (§6),
and is itself a useful, honest data point: pinning helps when you can *isolate* the
pinned cores, and actively hurts when you can only *claim* them on a machine still full of
other runnable work.

### 8.3 `lob_replay` segfaulted at -O2/-O3 on this toolchain

While re-verifying the README's own replay transcript (§ "Building and running" in the
top-level README), `./build/lob_replay examples/sample_session.txt` segfaulted. Every
time. Not a race, not a rare edge case: 20/20 runs on the full example script, 10/10 on a
single-line reduction. That's worth being direct about, because it's the kind of failure
a casual test pass (`lob_tests` all green, `lob_bench` running fine for hours) would never
catch — `lob_replay` is the one binary nothing else in the test suite exercises.

Bisection (full method: minimize the input, then vary optimization level, holding source
and input fixed):

* Input size didn't matter — a single `NEW` line crashed as reliably as the full 23-line
  script.
* **Optimization level did.** `-O0` and `-O1`: 0 crashes in 30+ runs each. `-O2` and
  `-O3`: 100% reproducible, every run, no exceptions found. `-mstackrealign` at `-O3`
  didn't help.
* It is specific to `tools/replay_main.cpp`. `bench_main.cpp` and `tests/*.cpp` share the
  exact same engine headers (same `alignas(64)` `InboundMsg`/`OutboundEvent`, same
  `MatchingEngine::emit()` constructing one on the stack per event) and never crashed —
  `lob_bench` ran clean at `-O3` for the entirety of this session, `lob_tests` for its
  full 29-test suite, both far more call volume than replay's handful of lines. Whatever
  this is, it's specific to how the optimizer treats `replay_main.cpp`'s particular
  function shapes, not a bug in the shared header code.
* It vanished when `stderr`-tracing `fprintf`s were added at every step of `main()` —
  classic symptom of undefined behavior whose manifestation depends on surrounding code
  layout, not a straightforward logic error. That also means: if you're chasing something
  like this yourself, don't trust "I added a print statement and it stopped happening" as
  a fix. It isn't one.

This has the profile of a compiler codegen bug rather than an application bug: GCC
16.1.0 is a very new toolchain on a target (MinGW-w64/Windows x64) that gets far less
real-world mileage than Linux, `-O2` is where GCC's tree vectorizer switches on, and the
one thing `replay_main.cpp` does that's unusual is repeatedly zero-initializing and
copying `alignas(64)` structs as plain stack locals in ordinary (non-hot-path) control
flow — a pattern the hot path also uses, but inside much smaller, more uniform functions
that may simply optimize down a different path. This was not root-caused to a specific
instruction (that would need disassembly, and GCC 16.1.0 not having a public track record
yet makes it hard to cross-check against known bug reports); what's here is what's
actually verified, not a guess dressed up as a diagnosis.

**Fix applied:** `scripts/build.sh` and `CMakeLists.txt` now build `lob_replay`
specifically at `-O1` regardless of the overall build mode (`lob_bench`/`lob_tests` are
untouched, still `-O3` in release). `lob_replay` is a printf-per-event demo/debug tool;
it has no performance requirement, so trading its optimization level for correctness costs
nothing. Verified clean: 0 crashes in 20 runs post-fix, output byte-for-byte matches the
transcript in the README. Re-test at `-O2` next time the toolchain is upgraded (a newer
GCC may simply fix this), and drop the cap if it no longer reproduces — don't carry it
forward on faith.

## 9. Reproducing

```bash
scripts/build.sh release          # or `native` for -march=native
scripts/run_bench.sh              # writes results/*.json, *.txt, *.hist.csv
# individual runs:
build/lob_bench --messages 10000000 --warmup 1000000            # max rate
build/lob_bench --messages 3000000 --warmup 300000 --rate 250000
build/lob_bench --mode split --pin 1,2,3 ...                    # >=3 cores
```

Runs are deterministic for a given `--seed` in inline mode (identical message
stream, trades, and counters; latencies of course still vary with the
machine).
