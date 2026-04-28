# InferenceGateway — Architecture

> A C++ request router for LLM serving. Multiple backend replicas; one
> dispatch thread; load-aware routing (power-of-two-choices); async
> batch coalescing for cheap requests; Prometheus latency tracking.
> Sub-10ms p99 scheduling overhead at 8K simulated req/s.

## Wire protocol

OpenAI-compatible HTTP/JSON. Clients POST to `/v1/completions` (or
`/v1/chat/completions`); the gateway forwards to one of the backend
replicas and proxies the response. Streaming responses pass through
unchanged via chunked transfer.

The original resume language said "gRPC frontend"; this rewrite uses
HTTP/JSON because (a) every real LLM serving stack we benchmark
against (vLLM, sglang, llama.cpp's server, TGI) speaks
OpenAI-compatible HTTP, (b) it's directly droppable in front of those
backends, and (c) the routing logic — which is the actual interesting
part — is identical under either transport. A protobuf service file is
sketched in `proto/inference.proto` for posterity.

## Top-level diagram

```
   client(s)                                backends (4 replicas)
   ─────────                                ───────────────────
       │                                          ▲
       │ POST /v1/completions                     │
       ▼                                          │
   ┌─────────────────────────────┐                │
   │   HTTP listener (cpp-       │                │
   │   httplib, thread pool)     │                │
   └──────────────┬──────────────┘                │
                  │ enqueue                       │
                  ▼                               │
   ┌─────────────────────────────┐                │
   │   Scheduler                 │   p2c-routed   │
   │   - request queue           ├────────────────┤
   │   - per-backend in-flight   │  HTTP/1.1      │
   │     counters                │  keep-alive    │
   │   - dispatch loop           │                │
   └──────────────┬──────────────┘                │
                  │                               │
                  ▼                               │
        ┌────────────────────┐                    │
        │  HTTPClient pool   │────────────────────┘
        │  per-backend       │
        │  conn keep-alive   │
        └────────────────────┘

   ┌────────────────┐
   │ /metrics       │  Prometheus exposition (rolling window
   │ /healthz       │  histograms; p50/p95/p99 per backend)
   │ /v1/cluster    │  gateway view of backend health
   └────────────────┘
```

## Scheduler

One dispatch thread; client threads enqueue. The dispatcher pops a
request, picks a backend, hands off to the per-backend client.

### Routing policies

- **round_robin** — baseline.
- **p2c** — power of two choices: pick two backends at random, send
  to the one with the lower in-flight count. This is the default.
  Empirically within a few percent of optimal under heterogeneous
  request sizes, with much smaller variance than round-robin.
- **least_loaded** — pick the global minimum. Slightly better than
  p2c when the request distribution is bimodal, but the extra
  scan is O(N) and locks the in-flight map for longer.

The choice of policy is a flag on startup; it never changes mid-run
(a flag flip would race with in-flight requests).

### Batching

For "small" backends (configured by URL prefix), the scheduler
optionally coalesces N requests within a `batch_window_us` (default
500 µs) into a single backend call, parses the array response, and
fans the results back to the originating clients. This is opt-in
because it only helps when the backend explicitly supports batched
inputs (vLLM's `/v1/completions` does; chat completions doesn't).

## Async I/O

cpp-httplib for the listener and the per-backend clients. We
considered Boost.Beast for the cross-platform async story but the
maintenance overhead wasn't worth it for a portfolio-scale system.
The thread pool is 32 workers by default; the dispatch thread is its
own dedicated thread with a lock-free MPSC queue
(`folly::ProducerConsumerQueue` analog — written by hand in
`internal/concurrent/spsc.h` so we don't pull in folly).

## Backend health

Each backend replica is polled at `/v1/models` every 5 seconds. A
backend that fails twice in a row is marked unhealthy and skipped by
the router until a successful poll. This is intentionally crude —
inference gateways usually defer health to a service mesh, but for a
self-contained demo it's the difference between "router blocks on a
dead backend" and "router routes around it".

## Metrics

Hand-rolled Prometheus exposition (`/metrics`). No prometheus-cpp
dependency; the cost is ~150 lines and the win is zero transitive
deps. Histograms for request latency are bucketed exponentially
(0.5/1/2/5/10/20/50/100/200/500/1000 ms). Per-backend counters and
gauges:

```
ig_requests_total{backend="b0",code="200"}
ig_request_duration_seconds_bucket{backend="b0",le="0.005"}
ig_inflight{backend="b0"}
ig_backend_healthy{backend="b0"}
ig_scheduler_queue_depth
ig_scheduler_overhead_microseconds_bucket{le="100"}
```

## Performance targets

- Sustain **8 000 req/s** across 4 simulated backends.
- Scheduler overhead (enqueue → dispatch → backend call dispatch)
  **p99 < 10 ms** at saturation.
- Memory < 200 MB resident at sustained load.

These numbers are measured by `cmd/loadgen` against `cmd/fakebackend`
(which echoes a deterministic 200-token completion after a configurable
delay). Single-machine numbers; the bench is part of CI as a smoke
test (lower req rate to keep CI runtime down).

## Build

CMake. Single dep is cpp-httplib (header-only, vendored as a
single-include in `third_party/httplib.h`); nlohmann/json (also
header-only, vendored). C++20 minimum.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

## Tests

- **scheduler** — request enqueue+dispatch, backend selection under
  each policy, in-flight bookkeeping correctness under concurrent
  pop+complete.
- **histogram** — bucket counts, percentile estimation, monotonicity.
- **router** — p2c convergence vs round-robin under heterogeneous
  request sizes (table-driven simulation, no real I/O).
- **backend health** — polling state machine; healthy ↔ unhealthy
  transitions on consecutive successes/failures.
- **integration** — loadgen + fakebackend at moderate rate; assert
  scheduler-overhead histogram p99 < threshold.

## Non-goals

- **Real model loading.** This is a router, not a runtime. Backends
  are external; we only proxy.
- **Stateful streaming session affinity.** A single connection's
  stream sticks to one backend, but two connections from the same
  client may go to different backends. Real production setups bolt
  on session-aware routing; out of scope.
- **mTLS, auth, rate-limiting.** Reverse-proxy concerns; expect a
  Envoy/nginx in front.
