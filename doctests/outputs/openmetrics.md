# Serving OpenMetrics by Scraping Modules

The `openmetrics` module serves an [OpenMetrics](https://prometheus.io/docs/specs/om/open_metrics_spec/)
`/metrics` endpoint for a logos.dev node. It is a **pure passthrough**: you
start it with a config JSON listing the modules to scrape, it stands up an HTTP
server, and on each scrape it calls every listed module's `collectMetrics()`
and renders the aggregated result as OpenMetrics text.

The interesting part is *where* those `collectMetrics()` calls happen. The HTTP
server ([libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/)) runs on
its own thread, but Logos inter-module calls (Qt Remote Objects) only work on
the module's main/event-loop thread. The module stays **pure C++** by relying
on the SDK to marshal those calls onto the owner thread — so a scrape triggered
from the HTTP thread reaches the other modules and comes back.

This doc-test proves the whole thing end-to-end on the commit under test:

1. Create three **provider** modules inline. Two — `temperature_module` and
   `request_counter_module` — implement the structured `collectMetrics()`
   convention (a gauge and a counter apiece). The third, `build_info_module`,
   instead returns an **already-rendered** OpenMetrics document via
   `collectOpenMetricsText()` — the scraper parses and merges that text like
   any other source.
2. Build the `openmetrics` scraper from **this repo**, and all three providers,
   then run them under a `logosctl` daemon.
3. Start the server (telling it to call `collectOpenMetricsText()` for
   `build_info_module` via `"format": "text"`), `curl /metrics`, and assert the
   aggregated OpenMetrics document — every series, structured or text-sourced,
   carrying a `module="<name>"` label.

A green run means the worker-thread scrape reached every provider, parsed the
rendered-text source, and rendered one valid OpenMetrics document.

**What you'll build:** The `openmetrics` scraper (built from this repo) plus three inline provider modules — two structured (`collectMetrics()`) and one that returns rendered OpenMetrics text (`collectOpenMetricsText()`) — run together in `logosctl`; an HTTP scrape of `/metrics` drives a collection call into each provider from the server thread.

**What you'll learn:**

- The `collectMetrics()` convention a module implements to be scrapeable
- The `collectOpenMetricsText()` convention for modules that already render OpenMetrics, and how the scraper parses + merges that text
- How to pick the collection method per module with the `"format"` config selector
- How `openmetrics` aggregates several modules' metrics into one OpenMetrics document
- How a module can serve HTTP and call other modules from the server thread (the SDK marshals the IPC)
- How to run a scraper + providers under a `logosctl` daemon and scrape with `curl`

## Prerequisites

- **Nix** with flakes enabled. Install from [nixos.org](https://nixos.org/download.html), then enable flakes:

```bash
mkdir -p ~/.config/nix
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Verify: `nix flake --help >/dev/null 2>&1 && echo "Flakes enabled"`

- **git** — nix flakes only see files tracked by git.
- **curl** — to scrape the endpoint.
- A Linux or macOS machine.

---

## Step 1: Create three metrics providers

A module becomes scrapeable by implementing one of two convention methods:

```cpp
LogosMap    collectMetrics();         // structured: { "metrics": [ {name,type,help,value,labels?}, ... ] }
std::string collectOpenMetricsText(); // already-rendered OpenMetrics text
```

The first two providers below use the structured form; the third
(`build_info_module`) returns rendered text. All three are ordinary
`universal` (pure-C++) modules — one plain class each, no Qt, no
inter-module calls. They just report numbers.

### 1.1 temperature_module/metadata.json

`interface: universal`, no dependencies — a leaf provider.

```json
{
  "name": "temperature_module",
  "version": "1.0.0",
  "type": "core",
  "category": "monitoring",
  "description": "Example metrics provider: a temperature gauge and a readings counter",
  "main": "temperature_module_plugin",
  "interface": "universal",
  "dependencies": [],

  "nix": {
    "packages": { "build": [], "runtime": [] },
    "external_libraries": [],
    "cmake": { "find_packages": [], "extra_sources": [] }
  }
}
```

### 1.2 temperature_module/CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(TemperatureModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()

logos_module(
    NAME temperature_module
    SOURCES
        src/temperature_module_impl.h
        src/temperature_module_impl.cpp
)
```

### 1.3 temperature_module/flake.nix

```nix
{
  description = "Example metrics provider: temperature";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
```

### 1.4 temperature_module/src/temperature_module_impl.h

A plain class. `collectMetrics()` returns a `LogosMap` (nlohmann::json)
in the convention shape — included via `<logos_json.h>`.

```cpp
#pragma once

#include <cstdint>

#include <logos_json.h>  // LogosMap / LogosList

// A leaf metrics provider. openmetrics calls collectMetrics() on it
// during each scrape.
class TemperatureModuleImpl {
public:
    /// Metrics for this module, in the collectMetrics() convention:
    /// { "metrics": [ { name, type, help, value, labels? }, ... ] }.
    LogosMap collectMetrics();

private:
    int64_t m_readings = 0;
};
```

### 1.5 temperature_module/src/temperature_module_impl.cpp

A gauge (with a label) and a counter that ticks once per scrape.

```cpp
#include "temperature_module_impl.h"

LogosMap TemperatureModuleImpl::collectMetrics()
{
    ++m_readings;

    LogosList metrics = LogosList::array();
    metrics.push_back({
        {"name",   "room_temperature_celsius"},
        {"type",   "gauge"},
        {"help",   "Current room temperature"},
        {"value",  21},
        {"labels", {{"sensor", "core"}}},
    });
    metrics.push_back({
        {"name",  "sensor_readings_total"},
        {"type",  "counter"},
        {"help",  "Total sensor readings taken"},
        {"value", m_readings},
    });
    return {{"metrics", metrics}};
}
```

### 1.6 request_counter_module/metadata.json

A second provider, so the scrape aggregates more than one module.

```json
{
  "name": "request_counter_module",
  "version": "1.0.0",
  "type": "core",
  "category": "monitoring",
  "description": "Example metrics provider: an HTTP request counter and an in-flight gauge",
  "main": "request_counter_module_plugin",
  "interface": "universal",
  "dependencies": [],

  "nix": {
    "packages": { "build": [], "runtime": [] },
    "external_libraries": [],
    "cmake": { "find_packages": [], "extra_sources": [] }
  }
}
```

### 1.7 request_counter_module/CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(RequestCounterModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()

logos_module(
    NAME request_counter_module
    SOURCES
        src/request_counter_module_impl.h
        src/request_counter_module_impl.cpp
)
```

### 1.8 request_counter_module/flake.nix

```nix
{
  description = "Example metrics provider: request counter";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
```

### 1.9 request_counter_module/src/request_counter_module_impl.h

```cpp
#pragma once

#include <cstdint>

#include <logos_json.h>  // LogosMap / LogosList

class RequestCounterModuleImpl {
public:
    /// Metrics for this module, in the collectMetrics() convention.
    LogosMap collectMetrics();

private:
    int64_t m_requests = 0;
};
```

### 1.10 request_counter_module/src/request_counter_module_impl.cpp

A labelled counter (jumps by 7 each scrape) and an in-flight gauge.

```cpp
#include "request_counter_module_impl.h"

LogosMap RequestCounterModuleImpl::collectMetrics()
{
    m_requests += 7;

    LogosList metrics = LogosList::array();
    metrics.push_back({
        {"name",   "http_requests_total"},
        {"type",   "counter"},
        {"help",   "Total HTTP requests handled"},
        {"value",  m_requests},
        {"labels", {{"method", "GET"}}},
    });
    metrics.push_back({
        {"name",  "inflight_requests"},
        {"type",  "gauge"},
        {"help",  "Requests currently in flight"},
        {"value", 3},
    });
    return {{"metrics", metrics}};
}
```

### 1.11 build_info_module/metadata.json

The third provider — it already speaks OpenMetrics, so it returns rendered text.

```json
{
  "name": "build_info_module",
  "version": "1.0.0",
  "type": "core",
  "category": "monitoring",
  "description": "Example metrics provider that returns rendered OpenMetrics text",
  "main": "build_info_module_plugin",
  "interface": "universal",
  "dependencies": [],

  "nix": {
    "packages": { "build": [], "runtime": [] },
    "external_libraries": [],
    "cmake": { "find_packages": [], "extra_sources": [] }
  }
}
```

### 1.12 build_info_module/CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.14)
project(BuildInfoModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()

logos_module(
    NAME build_info_module
    SOURCES
        src/build_info_module_impl.h
        src/build_info_module_impl.cpp
)
```

### 1.13 build_info_module/flake.nix

```nix
{
  description = "Example metrics provider: rendered OpenMetrics text";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
```

### 1.14 build_info_module/src/build_info_module_impl.h

Instead of `collectMetrics()`, it implements `collectOpenMetricsText()`,
returning a finished OpenMetrics document as a `std::string`.

```cpp
#pragma once

#include <string>

// A provider that already speaks OpenMetrics: it returns a rendered
// document rather than a structured map. The scraper is told to call
// collectOpenMetricsText() for it (format: "text"), then parses and
// merges the result like any other source.
class BuildInfoModuleImpl {
public:
    /// A rendered OpenMetrics document (# HELP/# TYPE/samples/# EOF).
    std::string collectOpenMetricsText();
};
```

### 1.15 build_info_module/src/build_info_module_impl.cpp

A build-info gauge (with a label), an uptime counter, and a latency
**histogram** — rendered as text. The histogram is the interesting case:
its `_bucket`/`_sum`/`_count` series are one family, and the scraper
re-groups them under a single `# HELP`/`# TYPE` after parsing. Note it
emits no `module=` label — the scraper injects the authoritative one.

```cpp
#include "build_info_module_impl.h"

std::string BuildInfoModuleImpl::collectOpenMetricsText()
{
    return
        "# HELP build_info Build information\n"
        "# TYPE build_info gauge\n"
        "build_info{version=\"1.2.3\"} 1\n"
        "# HELP service_uptime_seconds Seconds since process start\n"
        "# TYPE service_uptime_seconds counter\n"
        "service_uptime_seconds_total 123\n"
        "# HELP request_latency_seconds Request latency\n"
        "# TYPE request_latency_seconds histogram\n"
        "request_latency_seconds_bucket{le=\"0.1\"} 1\n"
        "request_latency_seconds_bucket{le=\"0.5\"} 3\n"
        "request_latency_seconds_bucket{le=\"+Inf\"} 4\n"
        "request_latency_seconds_sum 0.35\n"
        "request_latency_seconds_count 4\n"
        "# EOF\n";
}
```

---

## Step 2: Build the scraper and the providers

Build the `openmetrics` module straight from this repo's `#lgx` output (the
`` is pinned by the doc-test runner to the commit under test), and
build each provider's `.lgx`. Nix flakes only see git-tracked files, so
initialise a repo in each provider first.

### 2.1 Initialise git repos for the providers

```bash
(cd temperature_module && git init -q && git add -A)
(cd request_counter_module && git init -q && git add -A)
(cd build_info_module && git init -q && git add -A)

```

### 2.2 Build the openmetrics scraper from this repo

Its own `flake.lock` already pins the toolchain it needs (a
`logos-cpp-sdk` with worker-thread-safe inter-module calls), so no
overrides are required here.

```bash
# From a checkout this is simply: nix build '.#lgx' -o openmetrics-lgx
nix build 'github:logos-co/openmetrics-module#lgx' -o openmetrics-lgx
```

The scraper package is under `./openmetrics-lgx/`:

```bash
ls openmetrics-lgx/*.lgx
```

### 2.3 Build temperature_module

```bash
nix build 'path:./temperature_module#lgx' -o temperature-lgx
```

```bash
ls temperature-lgx/*.lgx
```

### 2.4 Build request_counter_module

```bash
nix build 'path:./request_counter_module#lgx' -o request-counter-lgx
```

```bash
ls request-counter-lgx/*.lgx
```

### 2.5 Build build_info_module

```bash
nix build 'path:./build_info_module#lgx' -o build-info-lgx
```

```bash
ls build-info-lgx/*.lgx
```

---

## Step 3: Build the runtime and install all three modules

Build `logosctl`, start its daemon, then install the scraper and the three
providers into the daemon's session.

### 3.1 Build logosctl

```bash
nix build 'github:logos-co/logos-logoscore-cli#ctl' --out-link ./logos
```

### 3.2 Start the daemon

The session directory, `./session`, holds the daemon's config, its
installed modules and their data. `--detach` returns once the daemon
accepts commands.

```bash
logosctl daemon start --detach
```

### 3.3 Install the scraper and the providers

Paths ending in `.lgx` install from disk, with no catalog involved, and
one command takes all four. Local builds are unsigned, which the
session's default policy accepts with a warning. `-y` confirms, since
there is no terminal to ask on.

```bash
logosctl install openmetrics-lgx/*.lgx temperature-lgx/*.lgx request-counter-lgx/*.lgx build-info-lgx/*.lgx -y
```

### 3.4 Confirm all four are installed

```bash
logosctl package ls
```

---

## Step 4: Run it and scrape /metrics

Load the providers and the scraper, then `start` the
scraper with a config JSON naming the three modules to scrape — the two
structured ones as bare strings, and `build_info_module` as an object with
`"format": "text"` so the scraper calls `collectOpenMetricsText()` for it. A
single `curl` then drives a collection call into each provider — from the
HTTP server thread — and returns the aggregated OpenMetrics document.

### 4.1 Load the providers

```bash
logosctl module load temperature_module
logosctl module load request_counter_module
logosctl module load build_info_module
```

### 4.2 Load the scraper

```bash
logosctl module load openmetrics
```

### 4.3 Start the OpenMetrics server

`start` takes one config JSON: the port, and the list of modules to
scrape. Each entry is a bare string (collected via `collectMetrics()`)
or an object selecting the method — here `build_info_module` is given
`"format": "text"` so the scraper calls `collectOpenMetricsText()` and
parses the result. Passed as a single argument so the JSON reaches the
module intact.

```bash
logosctl call openmetrics start '{"port":9099,"modules":["temperature_module","request_counter_module",{"name":"build_info_module","format":"text"}]}'
```

```bash
sleep 1
```

### 4.4 Scrape /metrics

The request handler runs on the libmicrohttpd thread; for each configured
module it collects through the bound `metrics_source` interface —
`collectMetrics()` for the two structured providers, and
`collectOpenMetricsText()` for `build_info_module` — and the SDK marshals
those calls onto each module's owner thread. The rendered text from
`build_info_module` is parsed back and merged: every series, structured
or text-sourced, carries a `module="<name>"` label, counters render with
the OpenMetrics `_total` sample, and the document ends with `# EOF`. Note
`build_info` and `service_uptime_seconds_total` now carry
`module="build_info_module"` even though the provider's own text didn't
include it, and the histogram's `_bucket`/`_sum`/`_count` series are
re-grouped under a single `# TYPE request_latency_seconds histogram`:

```bash
curl http://127.0.0.1:9099/metrics
```

### 4.5 Check /health

```bash
curl http://127.0.0.1:9099/health
```

### 4.6 getInfo reports what it's serving

`getInfo()` returns its status as a JSON string, so `logosctl`
delivers it as an escaped string inside the `result` field.

```bash
logosctl call openmetrics getInfo
```

### 4.7 Stop the server

```bash
logosctl call openmetrics stop
```

### 4.8 Stop the daemon

```bash
logosctl daemon stop
```

### 4.9 Confirm the daemon has stopped

```bash
logosctl daemon status
```

---

## Recap

| Module | Implements | In the scrape |
| ------ | ---------- | ------------- |
| `temperature_module` | `collectMetrics()` → gauge + counter | `room_temperature_celsius{…}`, `sensor_readings_total{…}` |
| `request_counter_module` | `collectMetrics()` → counter + gauge | `http_requests_total{…}`, `inflight_requests{…}` |
| `build_info_module` | `collectOpenMetricsText()` → rendered text | `build_info{…}`, `service_uptime_seconds_total{…}`, `request_latency_seconds` histogram (parsed + merged) |
| `openmetrics` (this repo) | `start`/`stop`/`getInfo`/`scrape` + HTTP server | aggregates all three, serves `/metrics` |

The two structured counters tick on every scrape (`sensor_readings_total`,
`http_requests_total`), so re-running `curl` shows them climb. Each
collection call happened on the HTTP server thread and was marshaled onto
the target module's owner thread by the SDK — which is what lets the scraper
stay pure C++. The `build_info_module` case additionally proves the
rendered-text path: its `collectOpenMetricsText()` output was parsed back
into the structured shape and merged — picking up the
`module="build_info_module"` label it never emitted itself. A green run is
end-to-end evidence that the `openmetrics` module on this commit aggregates
structured and rendered-text sources and serves them correctly.
