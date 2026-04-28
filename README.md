# InferenceGateway

A C++ request router for LLM serving. Multiple backend replicas, one
dispatch thread, load-aware routing (power-of-two-choices), Prometheus
metrics. The interesting part is the scheduler; the HTTP layer exists
only to make the scheduler driveable.

## What's in here

- **`include/ig/router.h`** — pure-functional routing policies: round-
  robin, p2c, least-loaded, random. Stateless; takes a snapshot of
  per-backend `inflight` counters and returns an index. Table-driven
  unit tests assert convergence.
- **`include/ig/scheduler.h`** — single-thread dispatcher with an MPSC
  request queue. Records `enqueue → dispatch` overhead in a
  Prometheus-shaped histogram (`ig_scheduler_overhead_seconds`); the
  service-level objective is **p99 ≤ 10 ms** at saturation.
- **`include/ig/histogram.h`** — fixed-bucket histogram, lock-free for
  the single-writer case, Prometheus-compatible bucket layout.
- **`include/ig/backend_pool.h`** — atomic per-backend counters
  (in-flight, total requests, errors, latency histogram) and a tiny
  health state machine (mark-fail-after-2 / mark-success-resets).
- **`include/ig/metrics.h`** — hand-rolled Prometheus exposition. Zero
  deps; ~150 lines.
- **`src/main.cpp`** — the gateway: HttpServer → Scheduler →
  BackendPool → outbound HttpPost. ~200 lines.
- **`src/fakebackend.cpp`** — LLM-replica simulator that responds
  after a configurable random delay. Used by `loadgen` and CI.

## Quick start

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build

# Local 4-backend cluster:
./build/fakebackend --port 9001 &
./build/fakebackend --port 9002 &
./build/fakebackend --port 9003 &
./build/fakebackend --port 9004 &
./build/inferenceg --policy p2c \
  --backends http://127.0.0.1:9001,http://127.0.0.1:9002,http://127.0.0.1:9003,http://127.0.0.1:9004 &

./build/loadgen --port 9090 --d 10 --c 64
curl -s http://127.0.0.1:9090/v1/cluster | jq
curl -s http://127.0.0.1:9090/metrics | head -40
```

Or with Docker:

```bash
docker compose up --build -d
```

## Tests

```bash
ctest --test-dir build --output-on-failure
```

Covers: round-robin cycling, unhealthy-skip, least-loaded selection,
p2c-favors-lower-inflight (stochastic; 2000 trials), router reports
−1 when no backend healthy, histogram percentile + bucket count,
end-to-end scheduler dispatch, drop on all-unhealthy, scheduler-
overhead metric under load, Prometheus exposition shape.

CI also runs an integration smoke: gateway + 4 fakebackends + loadgen
+ `/metrics` + `/v1/cluster` round-trip.

## Why HTTP/JSON, not gRPC?

The original design said "C++ gRPC frontend." This rewrite uses
HTTP/JSON because every real LLM serving stack we care about (vLLM,
sglang, llama.cpp's `server`, TGI) speaks OpenAI-compatible HTTP, and
the routing logic — which is the actual interesting part — is
identical under either transport. A protobuf service file is
sketched in `proto/inference.proto` for posterity. Adding a gRPC
listener is a ~300-line delta on top of `src/main.cpp`.

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for the design writeup
(scheduler, routing policies, batching, metrics, performance targets)
and [`docs/DEPLOY.md`](./docs/DEPLOY.md) for the operator runbook.

## Companion projects

Part of an eleven-repo set; see
[github.com/SAY-5](https://github.com/SAY-5) for the others
(canvaslive, pluginforge, agentlab, payflow, queryflow, datachat,
distributedkv, jobagent, ticketsearch, netprobekit, releaseguard).

## License

MIT.
