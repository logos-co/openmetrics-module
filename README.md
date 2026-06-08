# openmetrics-module

A Logos module (`openmetrics`) that serves an [OpenMetrics](https://prometheus.io/docs/specs/om/open_metrics_spec/)
`/metrics` endpoint for a logos.dev node, so operators can scrape it with
Prometheus and build dashboards.

It is a **pure passthrough**: you start it with a config JSON listing the
modules to scrape, it stands up an HTTP server, and on each scrape it calls each
module's `collectMetrics()` and renders the aggregated result as OpenMetrics
text. It does **not** discover modules or read platform stats — it only queries
the modules you list.

Written as a **universal pure-C++ module** (no Qt in the module code). It uses
the **interface-dependencies** feature: instead of depending on any concrete
module, it declares a dependency on the `metrics_source` *interface* (the
`collectMetrics()` contract in [`interfaces/metrics_source.h`](interfaces/metrics_source.h))
and binds it to operator-chosen module names at runtime. The HTTP server is
[libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/), a small, well-known
embeddable HTTP server.

> **Threading / SDK requirement.** The HTTP server runs on its own thread, but
> Logos inter-module calls (Qt Remote Objects) only work on the module's
> main/event-loop thread. The module stays pure C++ by relying on the SDK to
> marshal those calls: `logos-cpp-sdk`'s `LogosAPIClient` transparently runs
> `getClient`/`invokeRemoteMethod`/`requestObject`/`onEvent` on the owner thread
> when called from a worker thread (see `logos_thread_marshal.h`). This module
> therefore requires that SDK support.

## How modules expose metrics

Any module that wants to be scraped implements one method by convention:

```cpp
LogosMap collectMetrics();   // universal C++
```

returning openmetrics-like fields:

```json
{
  "metrics": [
    { "name": "storage_blocks_total", "type": "counter", "help": "Total blocks stored", "value": 42 },
    { "name": "storage_peers_connected", "type": "gauge", "help": "Connected peers", "value": 7, "labels": { "protocol": "libp2p" } }
  ]
}
```

| Field    | Meaning                                                                       |
| -------- | ----------------------------------------------------------------------------- |
| `name`   | metric name (for counters, the OpenMetrics `_total` sample suffix is handled) |
| `type`   | `counter`, `gauge`, `histogram`, or `summary` (unknown/missing → `unknown`)   |
| `help`   | short description                                                             |
| `value`  | number (bools map to 1/0; numeric strings pass through)                       |
| `labels` | optional string→string label pairs                                            |

Every emitted series additionally carries a `module="<name>"` label. Modules
that don't implement `collectMetrics` (or that error) are skipped, so one bad
module never breaks a scrape. See [`doctests/openmetrics.test.yaml`](doctests/openmetrics.test.yaml)
for two minimal providers, built and scraped end-to-end.

## Module API

| Method | Signature | Purpose |
|--------|-----------|---------|
| `start` | `start(config: string) -> int64` | Parse config JSON and start the HTTP server. Returns 1 on success, 0 on failure. |
| `stop` | `stop() -> int64` | Stop the HTTP server. Returns 1 if stopped, 0 if not running. |
| `getInfo` | `getInfo() -> string` | `{"running": bool, "port": int, "modules": [...]}` |
| `scrape` | `scrape() -> string` | Collect + render the OpenMetrics document directly (handy for debugging). |

`start` config JSON:

```json
{ "port": 9090, "modules": ["storage_module", "chat_module", "blockchain_module"] }
```

## Usage

```bash
# Build the module (default output) or a ready-to-install package
nix build            # the plugin
nix build .#lgx      # a .lgx package

# Run under a logoscore daemon alongside the modules you want to scrape, then
# point openmetrics at them by name:
logoscore -D -m ./modules --config-dir /tmp/om
logoscore --config-dir /tmp/om load-module my_module
logoscore --config-dir /tmp/om load-module openmetrics
logoscore --config-dir /tmp/om call openmetrics start '{"port":9090,"modules":["my_module"]}'

# Scrape it
curl http://localhost:9090/metrics
```

For a complete, runnable example — two provider modules built and scraped
end-to-end — see the doc-test in [`doctests/`](doctests/) (run it with
`cd doctests && ./run.sh`).

### HTTP endpoints

| Endpoint | Response |
|----------|----------|
| `GET /metrics` | OpenMetrics text (`application/openmetrics-text; version=1.0.0`) |
| `GET /health`  | `ok` (liveness) |

## Layout

The module sources sit at the repo root, next to `flake.nix`/`flake.lock` (the
builder expects the flake alongside `metadata.json`).

```
.
├── flake.nix                   # builds the openmetrics module (default, #lgx, #install, …)
├── flake.lock
├── metadata.json               # interface: universal; interface_dependencies: metrics_source
├── CMakeLists.txt              # logos_module + libmicrohttpd via pkg-config
├── interfaces/metrics_source.h # the collectMetrics() contract (IMetricsSource)
├── src/
│   ├── openmetrics_impl.h/.cpp     # LogosModuleContext; start/stop/getInfo/scrape + MHD server
│   └── openmetrics_format.h/.cpp   # LogosMap → OpenMetrics exposition text
└── doctests/                   # literate end-to-end doc-test
    ├── openmetrics.test.yaml   # creates two providers inline, builds + scrapes openmetrics
    ├── run.sh                  # runs the doc-test and regenerates outputs/
    └── outputs/openmetrics.md  # rendered report (commands + actual output)
```

The example provider modules aren't committed — they're created inline by the
doc-test, which builds this module from the commit under test, runs it with the
providers under a logoscore daemon, and scrapes `/metrics`.
