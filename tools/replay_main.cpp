// ----------------------------------------------------------------------------
// replay_main.cpp -- deterministic session replay / interactive book explorer.
//
// Reads a plain-text command script (file argument, or stdin) and prints
// every event the engine emits plus book snapshots on demand. This is the
// "show your work" tool: any matching decision can be reproduced and
// inspected line by line.
//
// Commands:
//   NEW <id> BUY|SELL LIMIT|MARKET <price> <qty> GTC|IOC|FOK
//   CXL <id>
//   RPL <id> <price> <qty>
//   BOOK <depth>            print aggregated top-<depth> levels
//   ORDERS <price>          list the FIFO queue at a price level (both sides)
//   # comment / blank lines ignored
//
// Example: ./lob_replay examples/sample_session.txt
// ----------------------------------------------------------------------------
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "lob/matching_engine.hpp"

using namespace lob;

namespace {

struct PrintSink {
  void operator()(const OutboundEvent& e) {
    switch (e.type) {
      case EventType::Accepted:
        std::printf("  [%3" PRIu64 "] ACCEPTED  id=%" PRIu64 " %s px=%" PRId64
                    " qty=%" PRIu64 "\n",
                    e.seq, e.id, to_string(e.side), e.price, e.qty);
        break;
      case EventType::Rejected:
        std::printf("  [%3" PRIu64 "] REJECTED  id=%" PRIu64 " reason=%s\n",
                    e.seq, e.id, to_string(e.reason));
        break;
      case EventType::Fill:
        std::printf("  [%3" PRIu64 "] FILL      id=%" PRIu64 " vs=%" PRIu64
                    " %s px=%" PRId64 " qty=%" PRIu64 " leaves=%" PRIu64 "%s\n",
                    e.seq, e.id, e.counter, to_string(e.side), e.price, e.qty,
                    e.leaves, (e.flags & kFlagTaker) ? " (taker)" : " (maker)");
        break;
      case EventType::Canceled:
        std::printf("  [%3" PRIu64 "] CANCELED  id=%" PRIu64 " leaves=%" PRIu64
                    " reason=%s\n",
                    e.seq, e.id, e.leaves, to_string(e.reason));
        break;
      case EventType::Replaced:
        std::printf("  [%3" PRIu64 "] REPLACED  id=%" PRIu64 " px=%" PRId64
                    " qty=%" PRIu64 "\n",
                    e.seq, e.id, e.price, e.qty);
        break;
      case EventType::BookTop: {
        const Price ap = book_top_ask_price(e);
        std::printf("  [%3" PRIu64 "] BOOK_TOP  bid=", e.seq);
        if (e.price == kNoPrice) std::printf("--");
        else std::printf("%" PRId64 "x%" PRIu64, e.price, e.qty);
        std::printf(" ask=");
        if (ap == kNoPrice) std::printf("--");
        else std::printf("%" PRId64 "x%" PRIu64, ap, e.leaves);
        std::printf("\n");
        break;
      }
    }
  }
};

using Engine = MatchingEngine<PrintSink>;

void print_book(const Engine& eng, std::size_t depth) {
  std::printf("  ---- book (top %zu) ----\n", depth);
  eng.book().for_each_level(
      Side::Sell,
      [](Price p, const PriceLevel& l) {
        std::printf("    ASK %8" PRId64 "  qty=%-8" PRIu64 " orders=%u\n", p,
                    l.total, l.count);
      },
      depth);
  eng.book().for_each_level(
      Side::Buy,
      [](Price p, const PriceLevel& l) {
        std::printf("    BID %8" PRId64 "  qty=%-8" PRIu64 " orders=%u\n", p,
                    l.total, l.count);
      },
      depth);
}

void print_orders_at(const Engine& eng, Price px) {
  for (Side s : {Side::Buy, Side::Sell}) {
    const PriceLevel* l = eng.book().find_level(s, px);
    if (!l) continue;
    std::printf("  %s @ %" PRId64 " (FIFO front -> back):\n", to_string(s), px);
    for (const Order* o = l->head; o != nullptr; o = o->next) {
      std::printf("    id=%" PRIu64 " leaves=%" PRIu64 " orig=%" PRIu64 "\n",
                  o->id, o->leaves, o->orig);
    }
  }
}

bool parse_side(const std::string& s, Side& out) {
  if (s == "BUY") { out = Side::Buy; return true; }
  if (s == "SELL") { out = Side::Sell; return true; }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  Price levels = 100000;
  std::string path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--levels" && i + 1 < argc) levels = std::atoll(argv[++i]);
    else path = a;
  }

  std::ifstream file;
  std::istream* in = &std::cin;
  if (!path.empty()) {
    file.open(path);
    if (!file) {
      std::fprintf(stderr, "cannot open %s\n", path.c_str());
      return 1;
    }
    in = &file;
  }

  EngineConfig cfg;
  cfg.min_price = 1;
  cfg.max_price = levels;
  cfg.max_open_orders = 1u << 16;
  cfg.publish_bbo = true;
  cfg.stamp_events = false;

  PrintSink sink;
  Engine eng(cfg, sink);

  std::string line;
  std::size_t lineno = 0;
  while (std::getline(*in, line)) {
    ++lineno;
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;
    std::printf("> %s\n", line.c_str());

    if (cmd == "NEW") {
      OrderId id;
      std::string side_s, type_s, tif_s;
      Price px;
      Qty qty;
      Side side;
      if (!(ss >> id >> side_s >> type_s >> px >> qty >> tif_s) ||
          !parse_side(side_s, side)) {
        std::fprintf(stderr, "parse error line %zu\n", lineno);
        continue;
      }
      InboundMsg m{};
      m.type = MsgType::NewOrder;
      m.id = id;
      m.side = side;
      m.ord_type = (type_s == "MARKET") ? OrdType::Market : OrdType::Limit;
      m.tif = (tif_s == "IOC") ? Tif::IOC : (tif_s == "FOK") ? Tif::FOK : Tif::GTC;
      m.price = px;
      m.qty = qty;
      eng.process(m);
    } else if (cmd == "CXL") {
      OrderId id;
      if (!(ss >> id)) continue;
      InboundMsg m{};
      m.type = MsgType::Cancel;
      m.id = id;
      eng.process(m);
    } else if (cmd == "RPL") {
      OrderId id;
      Price px;
      Qty qty;
      if (!(ss >> id >> px >> qty)) continue;
      InboundMsg m{};
      m.type = MsgType::Replace;
      m.id = id;
      m.price = px;
      m.qty = qty;
      eng.process(m);
    } else if (cmd == "BOOK") {
      std::size_t depth = 5;
      ss >> depth;
      print_book(eng, depth);
    } else if (cmd == "ORDERS") {
      Price px;
      if (ss >> px) print_orders_at(eng, px);
    } else {
      std::fprintf(stderr, "unknown command '%s' line %zu\n", cmd.c_str(), lineno);
    }
  }

  const EngineStats& s = eng.stats();
  std::printf(
      "\nsession totals: msgs=%" PRIu64 " trades=%" PRIu64 " qty=%" PRIu64
      " rejects=%" PRIu64 " open=%zu\n",
      s.msgs, s.trades, s.traded_qty, s.rejected, eng.open_orders());
  return 0;
}
