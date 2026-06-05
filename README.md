# prometheus-metrics-module

A Logos module (`prometheus_metrics`) that serves a Prometheus `/metrics`
endpoint for a logos.dev node running in headless mode.

It is a **pure passthrough**: you start it with a config JSON listing the
modules to scrape, it stands up an HTTP server, and on each scrape it fans out
**concurrently** to those modules, calls each one's `collectMetrics()` method,
and renders the aggregated result as Prometheus exposition text. It does **not**
discover loaded modules or read any platform/core stats — it only queries the
modules you tell it to.

Written in Rust (`c-ffi` interface) using
[`logos-rust-sdk`](https://github.com/logos-co/logos-rust-sdk) for inter-module
IPC and [`tiny_http`](https://crates.io/crates/tiny_http) for the server.

## How modules expose metrics

Any module that wants to be scraped implements one method by convention:

```
collectMetrics() -> LogosMap
```

returning prometheus-like fields:

```json
{
  "metrics": [
    { "name": "storage_blocks_total", "type": "counter", "help": "Total blocks stored", "value": 42 },
    { "name": "storage_peers_connected", "type": "gauge", "help": "Connected peers", "value": 7, "labels": { "protocol": "libp2p" } }
  ]
}
```

- `name` — Prometheus metric name
- `type` — `counter`, `gauge`, `histogram`, or `summary` (unknown → `untyped`)
- `help` — short description
- `value` — number (bools map to 1/0; numeric strings pass through)
- `labels` — optional string→string label pairs

Every emitted series additionally carries a `module="<name>"` label identifying
its source. Modules that don't implement `collectMetrics` (or that error/time
out) are skipped silently, so one bad module never breaks a scrape.

## Module API

| Method | Signature | Purpose |
|--------|-----------|---------|
| `start` | `start(config: string) -> int64` | Parse config JSON and start the HTTP server. Returns 1 on success, 0 on failure. |
| `stop` | `stop() -> int64` | Stop the HTTP server. Returns 1 if stopped, 0 if not running. |
| `get_info` | `get_info() -> string` | `{"running": bool, "port": int, "modules": [...]}` |

`start` config JSON:

```json
{ "port": 9090, "modules": ["storage_module", "chat_module", "blockchain_module"] }
```

## Usage

```bash
# Build the module
nix build .#prometheus_metrics

# Run under logoscore alongside the modules you want to scrape
logoscore -m result -m <other-modules-dir> \
  -l prometheus_metrics,storage_module,chat_module \
  -c 'prometheus_metrics.start({"port":9090,"modules":["storage_module","chat_module"]})'

# Scrape it
curl http://localhost:9090/metrics

# Stop it
logoscore -c 'prometheus_metrics.stop()'
```

### HTTP endpoints

| Endpoint | Response |
|----------|----------|
| `GET /metrics` | Prometheus exposition text (`text/plain; version=0.0.4`) |
| `GET /health`  | `ok` (liveness) |

## Build & test

```bash
nix build              # build the module
nix flake check        # run the Rust unit tests (formatter coverage)
```

Local Rust iteration (outside Nix) needs the SDK staged so the
`../logos-rust-sdk-src` path dependency resolves:

```bash
git clone https://github.com/logos-co/logos-rust-sdk logos-rust-sdk-src
cd rust-lib && cargo test
```

## Status & validation

Validated end-to-end against a `logoscore` daemon:

- The module builds (Nix), loads, and exposes exactly `start` / `stop` / `get_info`.
- `start` parses its config JSON, binds the port, and the server serves
  `GET /metrics` (`text/plain; version=0.0.4`), `GET /health`, and 404s.
- `stop` releases the port; `get_info` reports `{running, port, modules}`.
- Rust unit tests cover the exposition formatter and the server lifecycle
  (`nix flake check` / `cargo test`).
- The `collectMetrics` convention works: `logoscore call metrics_demo collectMetrics`
  returns the expected `{"metrics": [...]}` payload.

> **Note:** live cross-module scraping (the server calling `collectMetrics` on
> another module) relies on the platform's capability/token layer to authorize
> inter-module calls. That requires `capability_module` to be functional and all
> components (logoscore, capability_module, the modules) built from a consistent
> SDK — the normal case in a real deployment/CI. Verify scraping with:
>
> ```bash
> logoscore -D -m result --config-dir /tmp/pm
> logoscore --config-dir /tmp/pm load-module metrics_demo
> logoscore --config-dir /tmp/pm load-module prometheus_metrics
> logoscore --config-dir /tmp/pm call prometheus_metrics start '{"port":9090,"modules":["metrics_demo"]}'
> curl localhost:9090/metrics
> ```

## Layout

```
.
├── metadata.json                 # module manifest (interface: c-ffi)
├── CMakeLists.txt                # links the Rust staticlib + logos-module-client
├── flake.nix                     # builds the staticlib then the Qt plugin
├── lib/                          # staged at build time (.a + header) — gitignored
└── rust-lib/
    ├── Cargo.toml / Cargo.lock
    ├── include/prometheus_metrics.h   # C header the codegen wraps into methods
    └── src/
        ├── lib.rs        # extern "C" entry points → methods start/stop/get_info
        ├── state.rs      # global server state (config + handle)
        ├── collector.rs  # concurrent fan-out to modules' collectMetrics()
        ├── formatter.rs  # LogosMap JSON → Prometheus exposition text
        └── server.rs     # tiny_http server (/metrics, /health)
```
