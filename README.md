# Faultline

Deterministic network chaos for testing how services behave when the network stops being friendly.

Faultline is a C++23 TCP proxy that injects latency, jitter, bandwidth limits, connection resets, timeouts, and temporary blackouts. It is designed as an infrastructure tool first: headless, scriptable, observable, and testable without a web interface.

## Why Faultline

Distributed systems rarely fail cleanly. A request may take five seconds instead of fifty milliseconds, a connection may reset halfway through a response, or bandwidth may collapse while every process remains healthy. Faultline makes those conditions reproducible on a developer machine and in CI.

Current capabilities:

- non-blocking full-duplex TCP forwarding;
- independent upstream and downstream policies;
- deterministic jitter and reset decisions;
- latency and one-shot blackout scheduling;
- token-bucket bandwidth limiting;
- idle timeouts and TCP resets;
- bounded queues and backpressure;
- graceful shutdown and half-close propagation;
- strict scenario validation;
- structured JSON lifecycle logs and metrics;
- HTTP health, scenario, state, and live metrics endpoints;
- live latency, jitter, and bandwidth updates without reconnecting clients;
- ordered time-based stages with per-stage fault policies;
- confirmed remote shutdown through the local control API;
- real loopback integration tests.

## How it fits into your system

Faultline sits between a client and a service that already exists. Instead of sending test traffic directly to the service, point the client at Faultline:

```text
client -> Faultline :8080 -> your service :3000
```

Your application does not need to link against Faultline or change its code. Faultline forwards the same TCP stream while applying the selected latency, jitter, bandwidth, blackout, timeout, and reset policies. Use the control dashboard or HTTP API to change policies during a run and inspect what happened.

Typical uses include testing retry and timeout behavior, observing degraded dependencies, reproducing slow-network bugs, validating graceful recovery, and running deterministic failure scenarios in CI.

## Build

Requirements: CMake 3.25+, a C++23 compiler, and a POSIX system.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

Validate a scenario:

```bash
./build/faultline check --config examples/unstable-api.conf
```

Start the proxy:

```bash
./build/faultline run --config examples/unstable-api.conf
```

The example listens on `0.0.0.0:8080` and forwards to `127.0.0.1:3000`.

Inspect the running process:

```bash
curl http://127.0.0.1:9090/healthz
curl http://127.0.0.1:9090/v1/metrics
curl http://127.0.0.1:9090/v1/scenario
curl http://127.0.0.1:9090/v1/lifecycle
curl http://127.0.0.1:9090/v1/connections
curl 'http://127.0.0.1:9090/v1/events?after=0&limit=100'
curl http://127.0.0.1:9090/v1/state
```

`/v1/connections` returns connecting sessions plus the current stage and stage elapsed time for every active proxied connection. The dashboard uses this data to display per-stage activity and progress without assuming that all connections share one global timeline.

`/v1/events` returns a bounded cursor-based history of lifecycle, connection, stage, blackout, reset, timeout, rejection, error, and live policy events. Consumers retain `next_after` and send it as the next `after` value. A `truncated` response means the cursor fell behind the in-memory history and older events are no longer available.

Start the local control dashboard:

```bash
python3 -m http.server 4173 --directory ui
```

Open `http://127.0.0.1:4173` and use the settings button if the control endpoint or bearer token differs from the defaults.

Change an active policy:

```bash
curl -X PUT \
  -H 'X-Faultline-Confirm: update' \
  'http://127.0.0.1:9090/v1/policies/upstream?latency_ms=250&jitter_ms=40&bandwidth_kbps=512'
```

Every field is optional, but at least one must be supplied. Updates affect new traffic on existing and future connections. Already queued chunks keep the delay calculated when they were accepted. A control API bound outside loopback requires a `control.token` value of at least 16 characters and `Authorization: Bearer <token>` on every `/v1/*` request.

Request a graceful shutdown:

```bash
curl -X POST -H 'X-Faultline-Confirm: shutdown' http://127.0.0.1:9090/v1/shutdown
```

## Docker deployment

Run Faultline in front of a service listening on port `3000` on the host machine:

```bash
docker compose up --build
```

For another upstream, provide its address when starting the stack:

```bash
FAULTLINE_UPSTREAM_HOST=host.docker.internal \
FAULTLINE_UPSTREAM_PORT=4000 \
docker compose up --build
```

Clients connect to Faultline on `127.0.0.1:8080`. The dashboard is available at `http://127.0.0.1:4173`. Stop and remove the stack with `docker compose down`.

## Docker demo

The optional demo includes a tiny echo server so Faultline can be tried without preparing another application. The echo server is test data and is not part of Faultline itself.

```bash
./scripts/demo.sh
```

Open `http://127.0.0.1:4173`. The script starts the regular stack with `compose.demo.yml`, which adds the echo service and points Faultline at it.

To verify proxied traffic from another terminal:

```bash
python3 demo/client.py
```

Stop the demo with `docker compose -f compose.yml -f compose.demo.yml down`.

## Example scenario

```ini
[scenario]
name=unstable-api
experiment_id=local-unstable-api
seed=42042

[proxy]
listen_host=0.0.0.0
listen_port=8080
upstream_host=127.0.0.1
upstream_port=3000

[control]
host=127.0.0.1
port=9090

[faults]
idle_timeout_ms=30000
reset_probability=0.02

[upstream]
latency_ms=40
jitter_ms=10
bandwidth_kbps=2048

[downstream]
latency_ms=180
jitter_ms=40
bandwidth_kbps=1024
```

For a staged run, use `examples/staged-api.conf`. Each `[stage.N]` section starts when a connection is created. Stages are evaluated in order; the final stage must use `duration_ms=0` to remain active. Set `scenario.experiment_id` when Militantyx needs to correlate the run with captured traffic; the identifier is returned by the control API and included in every lifecycle log.

## Scope

Faultline currently works at the TCP byte-stream layer. It does not claim to simulate IP packet loss, duplication, or reordering. Those features require a future Linux transport based on `tc/netem`, network namespaces, or NFQUEUE.

## Roadmap

- UDP and Linux transparent modes;
- packaged Militantyx integration adapters;
- packaged dashboard distribution and deeper disruption analytics.

## Benchmarks

Run the isolated Release benchmark suite with:

```bash
scripts/run-benchmarks.sh
```

Use `scripts/run-benchmarks.sh smoke` for a shorter harness check. Results are emitted as JSON Lines and cover latency accuracy, bandwidth accuracy, no-fault proxy overhead and process CPU time, and short-connection churn. Benchmarks are intentionally separate from the normal tests and make no universal performance claim. See [docs/BENCHMARKS.md](docs/BENCHMARKS.md) for the output contract and comparison limits.

## License

MIT

## Development environment

Faultline uses the same local quality-gate model as Militantyx: `.editorconfig`, project-level `clang-format` and `clang-tidy`, CMake with Ninja, format and test scripts, pre-commit and pre-push hooks, and a matching GitHub Actions workflow.

```bash
scripts/format.sh
scripts/verify.sh
scripts/install-hooks.sh
```
