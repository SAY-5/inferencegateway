# InferenceGateway — Deployment Guide

Single C++ binary `inferenceg`. No external runtime deps; Docker image
is ~30 MB (debian-slim base + statically-built bins). Companion bins:
`fakebackend` (LLM-replica simulator) and `loadgen`.

## Quickstart with Docker

```bash
docker compose up --build -d
sleep 2
curl http://localhost:9090/healthz
curl http://localhost:9090/v1/cluster
```

The compose file boots 4 fakebackend containers + 1 gateway container,
all wired together. Open `http://localhost:9090/` in a browser for the
mission-control monitor (or serve `web/` from your favorite static
server, since the gateway doesn't bundle the dashboard by default —
see below for a one-liner).

## From source

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build

# Spin up four fake backends + the gateway locally:
./build/fakebackend --port 9001 & ./build/fakebackend --port 9002 &
./build/fakebackend --port 9003 & ./build/fakebackend --port 9004 &
./build/inferenceg --policy p2c \
  --backends http://127.0.0.1:9001,http://127.0.0.1:9002,http://127.0.0.1:9003,http://127.0.0.1:9004

# Pump traffic:
./build/loadgen --port 9090 --d 10 --c 64
```

## Configuration

| flag           | default                                    | meaning |
|----------------|--------------------------------------------|---------|
| `--port`       | `9090`                                     | gateway listen port |
| `--listen`     | `0.0.0.0`                                  | bind address |
| `--backends`   | `http://127.0.0.1:9001,...,9004`           | comma-separated backend URLs |
| `--policy`     | `p2c`                                      | `round_robin` / `p2c` / `least_loaded` / `random` |
| `--workers`    | `32`                                       | HTTP-server thread pool |
| `--timeout`    | `30000`                                    | per-request backend timeout (ms) |

## Production checklist

- **Reverse proxy out front.** This binary is a router, not an edge
  server. Put nginx/Envoy in front for TLS, auth, rate-limiting,
  request size caps.
- **Pin policy = p2c** unless you have a strong reason not to.
  Round-robin is the worst choice under heterogeneous request sizes
  (which is *every* LLM workload). Least-loaded is theoretically
  better than p2c but the lock-held scan becomes a bottleneck above
  ~50K rps.
- **Backend health interval (5 s) is hardcoded**; for >100 backends
  raise it. The poller is single-threaded and does sequential calls.
- **Scrape `/metrics`.** Prometheus exposition is hand-rolled and
  vanilla; any scraper config will work. The gateway publishes
  `ig_scheduler_overhead_seconds` — that's the SLO metric to alert
  on (target p99 ≤ 10 ms).
- **No keep-alive yet.** Each backend forward opens a new TCP
  connection. At >5K rps you'll see TIME_WAIT exhaustion; either
  raise `net.ipv4.tcp_tw_reuse` or implement keep-alive in
  `HttpPost` (left as a documented v2 task).

## Operational runbook

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| /v1/cluster shows backend `down` | health poller saw 2 consecutive failures | check the backend itself; the gateway will rejoin it on the next successful poll |
| `ig_scheduler_overhead_seconds` p99 > 10ms | dispatcher saturated; backends too slow | scale backends; switch to `least_loaded` if request sizes are very bimodal |
| `ig_scheduler_dropped_total` rising | all backends marked unhealthy | inspect their logs; lower the health-failure threshold from 2 if the system is flapping |
| 502 to clients | backend HTTP call failed | check backend logs; raise `--timeout` if backends are merely slow, not broken |

## Scaling notes

Single-instance throughput is bounded by the dispatch thread + the
backends' aggregate capacity. To go beyond, run multiple gateway
replicas behind an L4 load balancer; each replica picks its own
backend independently, and that's fine — p2c is robust to the loss of
shared state.

For sticky sessions (e.g. KV-cache reuse) you'd add a session-affinity
hash on top of p2c; the gateway only routes within the consistent-hash
shard for that session. That's a planned v2 feature; the routing
interface is already shaped to accept it.
