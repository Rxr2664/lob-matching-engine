// ----------------------------------------------------------------------------
// bench_main.cpp -- the measurement harness.
//
// Pipeline (split mode, the default):
//
//   [feed thread] --InboundMsg--> (SPSC ring) --> [matching thread]
//        ^                                             |
//        |                                       OutboundEvent
//   dead-order feedback (SPSC ring)                    v
//        +------------------------ [event thread] <-- (SPSC ring)
//
// The matching thread stamps two per-message latencies into HDR-style
// histograms: PROC (start of process() -> end) and E2E (producer enqueue
// timestamp -> end of processing, i.e. including ring transit + queueing).
//
// Honesty notes, spelled out because benchmarks lie by default:
//  * --rate paces the producer to a fixed offered load; at max speed
//    (--rate 0) the harness is CLOSED-LOOP, so E2E latency includes
//    queueing at saturation and is not comparable to open-loop numbers.
//    docs/BENCHMARKING.md discusses coordinated omission.
//  * --mode inline runs the whole pipeline on one thread (still through the
//    rings). Use it on machines without enough cores to dedicate one per
//    stage; PROC numbers remain meaningful, throughput reflects one core.
//  * Warmup messages are excluded from every reported statistic.
//  * --hist-out dumps every histogram bucket so nothing is hidden behind a
//    percentile summary. --json emits the full config + results.
// ----------------------------------------------------------------------------
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lob/common.hpp"
#include "lob/feed.hpp"
#include "lob/latency.hpp"
#include "lob/matching_engine.hpp"
#include "lob/messages.hpp"
#include "lob/spsc_ring.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace lob;

namespace {

struct BenchConfig {
  std::uint64_t messages = 10'000'000;
  std::uint64_t warmup = 1'000'000;
  std::uint64_t rate = 0;             // msgs/sec offered load; 0 = max speed
  std::uint64_t seed = 42;
  std::uint64_t sample_every = 1;     // record latency for every k-th message
  std::size_t in_ring_pow = 16;
  std::size_t out_ring_pow = 17;
  std::size_t fb_ring_pow = 16;
  std::size_t max_open = 1u << 20;
  Price levels = 200000;              // band = [1, levels]
  bool publish_bbo = true;
  bool stamp_events = true;
  bool inline_mode = false;
  int pin_feed = -1, pin_engine = -1, pin_events = -1;
  std::string json_path;
  std::string hist_path;
};

struct EngineThreadResult {
  Histogram proc;      // process() duration
  Histogram e2e;       // producer enqueue -> processed
  std::uint64_t processed = 0;
  std::uint64_t measured = 0;        // processed after warmup
  std::uint64_t t_measure_start = 0;
  std::uint64_t t_end = 0;
  EngineStats stats{};
  std::size_t open_at_end = 0;
  std::size_t pool_in_use = 0;
};

struct EventThreadResult {
  std::uint64_t counts[8] = {0};
  std::uint64_t feedback_dropped = 0;
  std::uint64_t total = 0;
};

bool pin_to_cpu(int cpu) {
#if defined(__linux__)
  if (cpu < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(_WIN32)
  // Windows has no thread-count limit on affinity masks the way glibc's
  // cpu_set_t does, but DWORD_PTR is only 64 bits wide, matching the
  // realistic core counts this harness targets.
  if (cpu < 0 || cpu >= 64) return false;
  return SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu) != 0;
#else
  (void)cpu;
  return false;
#endif
}

// Sink used by the engine: spin-push into the outbound ring (never drops).
struct RingSink {
  SpscRing<OutboundEvent>& ring;
  std::uint64_t spins = 0;
  void operator()(const OutboundEvent& e) {
    while (!ring.try_push(e)) {
      ++spins;
      cpu_relax();
    }
  }
};

bool is_terminal(const OutboundEvent& e) {
  // Events after which an order id is definitively no longer open.
  if (e.type == EventType::Canceled) return true;
  if (e.type == EventType::Rejected) return true;
  if (e.type == EventType::Fill && e.leaves == 0) return true;
  return false;
}

std::uint64_t parse_u64(const char* s) { return std::strtoull(s, nullptr, 10); }

void usage(const char* argv0) {
  std::printf(
      "usage: %s [options]\n"
      "  --messages N       total messages to feed (default 10000000)\n"
      "  --warmup N         messages excluded from stats (default 1000000)\n"
      "  --rate R           paced offered load in msg/s, 0 = max (default 0)\n"
      "  --mode split|inline  threading mode (default split)\n"
      "  --levels P         price band [1..P] (default 200000)\n"
      "  --max-open N       order pool / index capacity (default 1048576)\n"
      "  --ring-pow K       inbound ring = 2^K slots (default 16)\n"
      "  --sample K         record latency every K-th msg (default 1)\n"
      "  --seed S           feed RNG seed (default 42)\n"
      "  --pin F,E,V        pin feed/engine/event threads to cpus\n"
      "  --no-bbo           disable BookTop market-data events\n"
      "  --no-event-ts      do not stamp ts on outbound events\n"
      "  --json PATH        write config+results as JSON\n"
      "  --hist-out PATH    dump full latency histograms as CSV\n",
      argv0);
}

bool parse_args(int argc, char** argv, BenchConfig& c) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) { usage(argv[0]); std::exit(2); }
      return argv[++i];
    };
    if (a == "--messages") c.messages = parse_u64(next());
    else if (a == "--warmup") c.warmup = parse_u64(next());
    else if (a == "--rate") c.rate = parse_u64(next());
    else if (a == "--levels") c.levels = static_cast<Price>(parse_u64(next()));
    else if (a == "--max-open") c.max_open = parse_u64(next());
    else if (a == "--ring-pow") c.in_ring_pow = parse_u64(next());
    else if (a == "--sample") c.sample_every = std::max<std::uint64_t>(1, parse_u64(next()));
    else if (a == "--seed") c.seed = parse_u64(next());
    else if (a == "--mode") c.inline_mode = (std::string(next()) == "inline");
    else if (a == "--no-bbo") c.publish_bbo = false;
    else if (a == "--no-event-ts") c.stamp_events = false;
    else if (a == "--json") c.json_path = next();
    else if (a == "--hist-out") c.hist_path = next();
    else if (a == "--pin") {
      int f = -1, e = -1, v = -1;
      std::sscanf(next(), "%d,%d,%d", &f, &e, &v);
      c.pin_feed = f; c.pin_engine = e; c.pin_events = v;
    } else if (a == "--help" || a == "-h") { usage(argv[0]); std::exit(0); }
    else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(argv[0]); return false; }
  }
  if (c.warmup >= c.messages) c.warmup = c.messages / 10;
  return true;
}

void print_hist(const char* name, const Histogram& h) {
  std::printf(
      "  %-14s n=%-10" PRIu64 " p50=%-8" PRIu64 " p90=%-8" PRIu64
      " p99=%-8" PRIu64 " p99.9=%-8" PRIu64 " max=%-10" PRIu64 " mean=%.0f ns\n",
      name, h.count(), h.percentile(50), h.percentile(90), h.percentile(99),
      h.percentile(99.9), h.max(), h.mean());
}

void write_json(const BenchConfig& c, const EngineThreadResult& er,
                const EventThreadResult& vr, double mps, double elapsed_s,
                std::uint64_t producer_spins) {
  std::FILE* f = std::fopen(c.json_path.c_str(), "w");
  if (!f) { std::perror("json open"); return; }
  auto hist_json = [&](const Histogram& h) {
    std::fprintf(f,
                 "{\"count\":%" PRIu64 ",\"p50\":%" PRIu64 ",\"p90\":%" PRIu64
                 ",\"p99\":%" PRIu64 ",\"p999\":%" PRIu64 ",\"max\":%" PRIu64
                 ",\"mean_ns\":%.1f}",
                 h.count(), h.percentile(50), h.percentile(90),
                 h.percentile(99), h.percentile(99.9), h.max(), h.mean());
  };
  std::fprintf(f, "{\n  \"config\":{");
  std::fprintf(f,
               "\"messages\":%" PRIu64 ",\"warmup\":%" PRIu64
               ",\"rate\":%" PRIu64 ",\"seed\":%" PRIu64
               ",\"sample_every\":%" PRIu64
               ",\"levels\":%" PRId64 ",\"max_open\":%zu,\"mode\":\"%s\""
               ",\"publish_bbo\":%s,\"stamp_events\":%s},\n",
               c.messages, c.warmup, c.rate, c.seed, c.sample_every, c.levels,
               c.max_open, c.inline_mode ? "inline" : "split",
               c.publish_bbo ? "true" : "false",
               c.stamp_events ? "true" : "false");
  std::fprintf(f,
               "  \"results\":{\"throughput_msgs_per_sec\":%.1f,"
               "\"measured_msgs\":%" PRIu64 ",\"elapsed_s\":%.4f,"
               "\"producer_backpressure_spins\":%" PRIu64 ",\n",
               mps, er.measured, elapsed_s, producer_spins);
  std::fprintf(f, "    \"proc_latency_ns\":");
  hist_json(er.proc);
  std::fprintf(f, ",\n    \"e2e_latency_ns\":");
  hist_json(er.e2e);
  const EngineStats& s = er.stats;
  std::fprintf(f,
               ",\n    \"engine\":{\"msgs\":%" PRIu64 ",\"accepted\":%" PRIu64
               ",\"rejected\":%" PRIu64 ",\"user_cancels\":%" PRIu64
               ",\"expired\":%" PRIu64 ",\"replaced\":%" PRIu64
               ",\"trades\":%" PRIu64 ",\"traded_qty\":%" PRIu64
               ",\"bbo_updates\":%" PRIu64 ",\"open_at_end\":%zu},\n",
               s.msgs, s.accepted, s.rejected, s.user_cancels, s.expired,
               s.replaced, s.trades, s.traded_qty, s.bbo_updates,
               er.open_at_end);
  std::fprintf(f,
               "    \"events\":{\"total\":%" PRIu64 ",\"accepted\":%" PRIu64
               ",\"rejected\":%" PRIu64 ",\"fills\":%" PRIu64
               ",\"canceled\":%" PRIu64 ",\"replaced\":%" PRIu64
               ",\"book_top\":%" PRIu64 ",\"feedback_dropped\":%" PRIu64 "}\n",
               vr.total, vr.counts[int(EventType::Accepted)],
               vr.counts[int(EventType::Rejected)],
               vr.counts[int(EventType::Fill)],
               vr.counts[int(EventType::Canceled)],
               vr.counts[int(EventType::Replaced)],
               vr.counts[int(EventType::BookTop)], vr.feedback_dropped);
  std::fprintf(f, "  }\n}\n");
  std::fclose(f);
  std::printf("wrote %s\n", c.json_path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  BenchConfig cfg;
  if (!parse_args(argc, argv, cfg)) return 2;

  EngineConfig ecfg;
  ecfg.min_price = 1;
  ecfg.max_price = cfg.levels;
  ecfg.max_open_orders = cfg.max_open;
  ecfg.publish_bbo = cfg.publish_bbo;
  ecfg.stamp_events = cfg.stamp_events;

  FeedConfig fcfg;
  fcfg.seed = cfg.seed;
  fcfg.min_price = 1;
  fcfg.max_price = cfg.levels;
  fcfg.start_mid = cfg.levels / 2;
  fcfg.max_tracked = cfg.max_open;

  auto in_ring = std::make_unique<SpscRing<InboundMsg>>(std::size_t(1) << cfg.in_ring_pow);
  auto out_ring = std::make_unique<SpscRing<OutboundEvent>>(std::size_t(1) << cfg.out_ring_pow);
  auto fb_ring = std::make_unique<SpscRing<OrderId>>(std::size_t(1) << cfg.fb_ring_pow);

  RingSink sink{*out_ring};
  auto engine = std::make_unique<MatchingEngine<RingSink>>(ecfg, sink);
  FeedGenerator feed(fcfg);

  auto er = std::make_unique<EngineThreadResult>();
  EventThreadResult vr;
  std::atomic<bool> engine_done{false};
  std::uint64_t producer_spins = 0;
  std::uint64_t producer_elapsed_ns = 0;

  std::printf("lob_bench: mode=%s messages=%" PRIu64 " warmup=%" PRIu64
              " rate=%" PRIu64 " levels=%" PRId64 " max_open=%zu seed=%" PRIu64
              " bbo=%d event_ts=%d sample=%" PRIu64 "\n",
              cfg.inline_mode ? "inline" : "split", cfg.messages, cfg.warmup,
              cfg.rate, cfg.levels, cfg.max_open, cfg.seed,
              int(cfg.publish_bbo), int(cfg.stamp_events), cfg.sample_every);

  // ---- producer body ------------------------------------------------------
  auto produce_one = [&](std::uint64_t& next_send_ns, std::uint64_t interval_ns) {
    // opportunistically absorb execution-report feedback
    OrderId dead;
    int drained = 0;
    while (drained < 64 && fb_ring->try_pop(dead)) {
      feed.on_order_dead(dead);
      ++drained;
    }
    InboundMsg m = feed.next();
    if (interval_ns) {
      while (now_ns() < next_send_ns) cpu_relax();
      next_send_ns += interval_ns;
    }
    m.ts_enqueue_ns = now_ns();
    while (!in_ring->try_push(m)) {
      ++producer_spins;
      cpu_relax();
      OrderId d;
      if (fb_ring->try_pop(d)) feed.on_order_dead(d);
    }
  };

  // ---- engine body --------------------------------------------------------
  auto engine_process = [&](const InboundMsg& m) {
    const std::uint64_t t0 = now_ns();
    engine->process(m);
    const std::uint64_t t1 = now_ns();
    ++er->processed;
    if (er->processed > cfg.warmup) {
      if ((er->processed % cfg.sample_every) == 0) {
        er->proc.record(t1 - t0);
        er->e2e.record(t1 - m.ts_enqueue_ns);
      }
      ++er->measured;
    }
    if (er->processed == cfg.warmup) {
      er->proc.reset();
      er->e2e.reset();
      er->t_measure_start = t1;
    }
  };

  auto finish_engine = [&]() {
    er->t_end = now_ns();
    if (er->t_measure_start == 0) er->t_measure_start = er->t_end;
    er->stats = engine->stats();
    er->open_at_end = engine->open_orders();
    engine_done.store(true, std::memory_order_release);
  };

  // ---- event consumer body ------------------------------------------------
  auto consume_event = [&](const OutboundEvent& e) {
    ++vr.total;
    ++vr.counts[static_cast<int>(e.type) & 7];
    if (is_terminal(e)) {
      if (!fb_ring->try_push(e.id)) ++vr.feedback_dropped;
    }
  };

  if (cfg.inline_mode) {
    // Everything on one thread, still flowing through the rings.
    const std::uint64_t t_prod0 = now_ns();
    std::uint64_t interval = cfg.rate ? 1'000'000'000ull / cfg.rate : 0;
    std::uint64_t next_send = now_ns();
    InboundMsg m;
    OutboundEvent e;
    for (std::uint64_t n = 0; n < cfg.messages; ++n) {
      produce_one(next_send, interval);
      while (in_ring->try_pop(m)) engine_process(m);
      while (out_ring->try_pop(e)) consume_event(e);
    }
    finish_engine();
    while (out_ring->try_pop(e)) consume_event(e);
    producer_elapsed_ns = now_ns() - t_prod0;
  } else {
    std::thread event_thr([&] {
      pin_to_cpu(cfg.pin_events);
      OutboundEvent e;
      for (;;) {
        if (out_ring->try_pop(e)) {
          consume_event(e);
        } else if (engine_done.load(std::memory_order_acquire) &&
                   out_ring->empty()) {
          break;
        } else {
          cpu_relax();
        }
      }
    });

    std::thread engine_thr([&] {
      pin_to_cpu(cfg.pin_engine);
      InboundMsg batch[256];
      bool done = false;
      while (!done) {
        const std::size_t n = in_ring->try_pop_batch(batch, 256);
        if (n == 0) { cpu_relax(); continue; }
        for (std::size_t i = 0; i < n; ++i) {
          if (batch[i].type == MsgType::Shutdown) { done = true; break; }
          engine_process(batch[i]);
        }
      }
      finish_engine();
    });

    std::thread feed_thr([&] {
      pin_to_cpu(cfg.pin_feed);
      const std::uint64_t t0 = now_ns();
      std::uint64_t interval = cfg.rate ? 1'000'000'000ull / cfg.rate : 0;
      std::uint64_t next_send = now_ns();
      for (std::uint64_t n = 0; n < cfg.messages; ++n) produce_one(next_send, interval);
      InboundMsg stop{};
      stop.type = MsgType::Shutdown;
      while (!in_ring->try_push(stop)) cpu_relax();
      producer_elapsed_ns = now_ns() - t0;
    });

    feed_thr.join();
    engine_thr.join();
    event_thr.join();
  }

  // ---- report -------------------------------------------------------------
  const double elapsed_s =
      double(er->t_end - er->t_measure_start) / 1e9;
  const double mps = elapsed_s > 0 ? double(er->measured) / elapsed_s : 0.0;
  const double offered =
      producer_elapsed_ns ? double(cfg.messages) / (double(producer_elapsed_ns) / 1e9)
                          : 0.0;

  std::printf("\n==== results (post-warmup) ====\n");
  std::printf("  engine throughput   %.0f msg/s  (%.3f M msg/s)\n", mps, mps / 1e6);
  std::printf("  offered rate        %.0f msg/s\n", offered);
  std::printf("  measured messages   %" PRIu64 "  over %.3f s\n", er->measured, elapsed_s);
  print_hist("proc latency", er->proc);
  print_hist("e2e latency", er->e2e);

  const EngineStats& s = er->stats;
  std::printf("\n==== engine counters (full run incl. warmup) ====\n");
  std::printf("  msgs=%" PRIu64 " accepted=%" PRIu64 " rejected=%" PRIu64
              " cancels=%" PRIu64 " expired=%" PRIu64 " replaced=%" PRIu64 "\n",
              s.msgs, s.accepted, s.rejected, s.user_cancels, s.expired,
              s.replaced);
  std::printf("  trades=%" PRIu64 " traded_qty=%" PRIu64 " bbo_updates=%" PRIu64
              " open_at_end=%zu\n",
              s.trades, s.traded_qty, s.bbo_updates, er->open_at_end);
  std::printf("  producer backpressure spins=%" PRIu64
              " outbound sink spins=%" PRIu64 "\n",
              producer_spins, sink.spins);
  std::printf("\n==== event counters ====\n");
  std::printf("  total=%" PRIu64 " accepted=%" PRIu64 " rejected=%" PRIu64
              " fills=%" PRIu64 " canceled=%" PRIu64 " replaced=%" PRIu64
              " book_top=%" PRIu64 " fb_dropped=%" PRIu64 "\n",
              vr.total, vr.counts[int(EventType::Accepted)],
              vr.counts[int(EventType::Rejected)],
              vr.counts[int(EventType::Fill)],
              vr.counts[int(EventType::Canceled)],
              vr.counts[int(EventType::Replaced)],
              vr.counts[int(EventType::BookTop)], vr.feedback_dropped);

  std::printf("\n==== book at end (top 5) ====\n");
  engine->book().for_each_level(
      Side::Sell,
      [](Price p, const PriceLevel& l) {
        std::printf("  ASK %8" PRId64 "  qty=%-8" PRIu64 " orders=%u\n", p,
                    l.total, l.count);
      },
      5);
  engine->book().for_each_level(
      Side::Buy,
      [](Price p, const PriceLevel& l) {
        std::printf("  BID %8" PRId64 "  qty=%-8" PRIu64 " orders=%u\n", p,
                    l.total, l.count);
      },
      5);

  if (!cfg.hist_path.empty()) {
    std::FILE* hf = std::fopen(cfg.hist_path.c_str(), "w");
    if (hf) {
      er->proc.dump_csv(hf, "proc");
      er->e2e.dump_csv(hf, "e2e");
      std::fclose(hf);
      std::printf("wrote %s\n", cfg.hist_path.c_str());
    }
  }
  if (!cfg.json_path.empty()) {
    write_json(cfg, *er, vr, mps, elapsed_s, producer_spins);
  }
  return 0;
}
