# Contributing — project notes for maintainers

## What this project is

A single-instrument **limit order book matching engine** in C++20:
price-time priority matching, wait-free SPSC ring buffers between a feed
thread, the matching thread, and an event thread, a zero-allocation hot path,
and log-linear latency histograms (p50/p99/p99.9). The claims it must keep
supporting:

- lock-free SPSC ring hand-off, allocation-free critical path
- sustained 237k+ msgs/sec (measured: 250k and 1M msg/s paced profiles on two
  independent machines now — original single shared cloud vCPU, and a
  Windows/MinGW re-verification in `--mode split`; see
  `docs/BENCHMARKING.md` §7-8 for both, `results/` for the current one's raw
  JSON/histograms)
- latency instrumentation that guided memory layout (see
  `docs/ARCHITECTURE.md` §8)

## Authoritative docs — read before changing behaviour

- `docs/ARCHITECTURE.md` — **the spec.** Message/event wire formats, exact
  matching semantics (validation order, FOK pre-scan, replace priority rules,
  event ordering guarantees), ring memory-model reasoning, data structures.
- `docs/BENCHMARKING.md` — methodology (warmup exclusion, proc vs e2e,
  coordinated omission, environment disclosure) and reference results.

The fuzz test's reference book (`tests/test_fuzz.cpp`) is written against
that spec. **Any semantic change must update the spec, the reference book,
and the scripted tests together** — if they disagree, one of them is wrong.

## Build / test / bench

```bash
scripts/build.sh release      # -O3 build into build/
scripts/build.sh native       # -O3 -march=native
scripts/build.sh debug        # ASan + UBSan; run the tests under this too
./build/lob_tests             # 29 tests, must all pass (release AND debug)
scripts/run_bench.sh          # 3 profiles -> results/*.json,*.txt,*.hist.csv
./build/lob_replay examples/sample_session.txt   # human-readable demo
```

CMake ≥3.16, g++ or clang with C++20. `ctest` works from `build/` (verified
2026-08-09: `cmake -S . -B build -G "MinGW Makefiles"` works fine with
MinGW-w64 GCC too, not just Linux/Makefiles).

**Platform gap (covered by CI):** `scripts/build.sh debug` (ASan+UBSan)
fails on a plain MinGW-w64 GCC toolchain (WinLibs build, no `libasan`/
`libubsan` shipped — link fails with `cannot find -lasan`). Tried swapping
in LLVM/clang as `CXX`; that build targets MSVC by default on Windows and
has no `<atomic>` etc. without a real Visual Studio C++ install, which
wasn't available either. Net effect: on Windows, only the `release`/`native`
leg of "must pass release AND debug" is currently verified — the ASan/UBSan
leg needs a Linux box (or WSL, or a full MSVC+clang-cl setup) to actually
run. Don't claim both legs pass on a Windows-only setup. The GitHub Actions
workflow (`.github/workflows/ci.yml`) runs the ASan/UBSan leg on Linux for
every PR, so that is where it gets verified.
**Also:** a failed `scripts/build.sh debug` run can silently delete a
previously-good `build/lob_bench` (the compiler unlinks its `-o` target on
failure, and the script writes straight to the real path, not a temp file)
— if a debug build fails, rebuild release before trusting `build/` again.

**Bigger platform gap, now fixed — read this before assuming "all tests
green" means "the project works":** on MinGW-w64 GCC 16.1.0 (Windows),
`lob_replay` built at `-O2`/`-O3` segfaulted on 100% of runs, including the
bundled `examples/sample_session.txt` — found only because someone actually
ran the replay tool by hand; `lob_tests`/`lob_bench` never exercise it and
were both green the whole time. `-O0`/`-O1` never crash. Root cause not
pinned to a specific instruction (looks like a GCC 16.1.0 codegen bug on
this target, not an application bug — see `docs/BENCHMARKING.md` §8.3 for
the full bisection); fix applied is pragmatic, not a diagnosis: both
`scripts/build.sh` and `CMakeLists.txt` now cap `lob_replay` specifically at
`-O1` (it's a printf-per-event demo tool, costs nothing). If you touch
either build file, keep that cap — removing it silently reintroduces a
100%-reproducible crash in the tool the README's own usage example runs.
**General lesson for this project:** build success and `lob_tests` passing
prove the header-only engine works; they say nothing about `tools/` or
`bench/` binaries that the test suite doesn't invoke. Actually *run* every
binary the README tells a user to run, on whatever platform you're
verifying, before calling it done.

On a machine with **≥3 physical cores**, benchmark with the real topology:
`--mode split --pin F,E,V` (feed/engine/event core ids), ideally with
isolated cores and the performance governor. `pin_to_cpu()` in
`bench/bench_main.cpp` now has both a Linux (`pthread_setaffinity_np`) and a
Windows (`SetThreadAffinityMask`) branch. **Caveat learned the hard way:**
`--pin` only helps if the cores it names are actually isolated (`isolcpus`
or equivalent); on a normal shared/hybrid-core Windows box, explicit pinning
made split-mode tail latency *worse* than leaving placement to the OS
scheduler — see `docs/BENCHMARKING.md` §8 before assuming `--pin` is free
upside on whatever machine you're running on.

## Layout

```
include/lob/   header-only engine: common, messages, spsc_ring, pool,
               flat_map, latency, order_book, matching_engine, feed
bench/         bench_main.cpp (all flags documented in --help)
tools/         replay_main.cpp (script-driven demo, PrintSink)
tests/         harness.hpp + 6 test files (ring stress, differential fuzz
               vs std::map reference book, zero-allocation proof, 16
               scripted engine scenarios)
docs/          ARCHITECTURE.md, BENCHMARKING.md
results/       committed measured JSON/txt/histogram CSVs (transparency)
scripts/       build.sh, run_bench.sh
```

## Hard invariants (tests enforce these — do not break)

1. Zero heap allocations on the hot path (`test_no_alloc.cpp` counts global
   `operator new`).
2. Exactly one producer and one consumer per ring; only release/acquire on
   the indices, slot writes never after the release store.
3. `InboundMsg` and `OutboundEvent` stay exactly 64 bytes, trivially
   copyable (`static_assert`ed).
4. Event `seq` is gap-free; maker Fill precedes taker Fill; execution price
   is always the maker's price; `Replaced` precedes any fills it triggers.
5. BookTop events are deduplicated (only on actual top-of-book change).
6. Determinism for a fixed `--seed` in inline mode.

## Style

- C++20, header-only engine, no external dependencies (tests use the tiny
  harness in `tests/harness.hpp`, not gtest).
- `.clang-format` at repo root; 2-space indent, 100 cols.
- Comments explain *why* (memory-model reasoning, layout decisions), not what.

## Good next tasks on real hardware

- Done 2026-08-09 on a Windows/i7-1280P box: re-ran in `--mode split`,
  refreshed the README table and `docs/BENCHMARKING.md` §8, and added
  Windows CPU-affinity support. Open follow-ups from that run:
  - Quantify the ~100ns proc-latency floor increase seen on MinGW/UCRT with
    `--sample`/`--no-event-ts` (§4) to confirm the `clock_gettime` overhead
    hypothesis in `docs/BENCHMARKING.md` §8.1.
  - Try `--mode split --pin` on an *isolated-core* Linux box (real
    `isolcpus`, not a shared/hybrid desktop) to get the pinned-and-isolated
    number the original "good next task" was actually after — this Windows
    run showed pinning helps only when the pinned cores are truly isolated.
- **Commit before every benchmark run** —
  `scripts/run_bench.sh` overwrites `results/*.json|txt|hist.csv` in place,
  and on 2026-08-09 that destroyed the original cloud-container run's raw
  files because no git history existed to recover them from (numbers
  survived in prose in README/BENCHMARKING.md; the raw histograms didn't).
  Don't repeat that: commit before every re-run, or the previous run's raw
  data is gone the moment the next one starts.
- Optional extensions listed in README "Scaling and limitations": multiple
  instruments (one engine per symbol), ITCH/OUCH-style codecs, snapshotting.
