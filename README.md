# LOB Engine — a limit order book matching engine in C++20

A single-instrument limit order book with strict price–time priority matching, fed through a
lock-free SPSC ring buffer, with an allocation-free matching path and per-message latency
histograms.
