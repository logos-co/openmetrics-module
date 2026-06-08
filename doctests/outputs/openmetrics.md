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

1. Create two **provider** modules inline — `temperature_module` and
   `request_counter_module` — each implementing the `collectMetrics()`
   convention (a gauge and a counter apiece).
2. Build the `openmetrics` scraper from **this repo**, and both providers, then
   run all three under a `logoscore` daemon.
3. Start the server, `curl /metrics`, and assert the aggregated OpenMetrics
   document — every series carrying a `module="<name>"` label.

A green run means the worker-thread scrape reached both providers and rendered
valid OpenMetrics.

**What you'll build:** The `openmetrics` scraper (built from this repo) plus two inline provider modules, run together in `logoscore`; an HTTP scrape of `/metrics` drives a `collectMetrics()` call into each provider from the server thread.

**What you'll learn:**

- The `collectMetrics()` convention a module implements to be scrapeable
- How `openmetrics` aggregates several modules' metrics into one OpenMetrics document
- How a module can serve HTTP and call other modules from the server thread (the SDK marshals the IPC)
- How to run a scraper + providers under a `logoscore` daemon and scrape with `curl`

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

## Step 1: Create two metrics providers

A module becomes scrapeable by implementing one method by convention:

```cpp
LogosMap collectMetrics();   // returns { "metrics": [ {name,type,help,value,labels?}, ... ] }
```

Both providers below are ordinary `universal` (pure-C++) modules — one plain
class each, no Qt, no inter-module calls. They just report numbers.

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

---

## Step 3: Build the runtime and install all three modules

Build `logoscore` and `lgpm`, seed the modules directory with the capability
module that ships with `logoscore`, then install the scraper and both
providers.

### 3.1 Build logoscore

```bash
nix build 'github:logos-co/logos-logoscore-cli' --out-link ./logos
```

### 3.2 Build lgpm

```bash
nix build 'github:logos-co/logos-package-manager#cli' -o lgpm
```

### 3.3 Seed the modules directory with the capability module

```bash
mkdir -p modules
cp -RL ./logos/modules/. ./modules/

```

### 3.4 Install the scraper

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file openmetrics-lgx/*.lgx
```

### 3.5 Install temperature_module

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file temperature-lgx/*.lgx
```

### 3.6 Install request_counter_module

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file request-counter-lgx/*.lgx
```

### 3.7 Confirm all three are installed

```bash
./lgpm/bin/lgpm --modules-dir ./modules list
```

---

## Step 4: Run it and scrape /metrics

Start the daemon, load the providers and the scraper, then `start` the
scraper with a config JSON naming the two modules to scrape. A single
`curl` then drives one `collectMetrics()` call into each provider — from the
HTTP server thread — and returns the aggregated OpenMetrics document.

### 4.1 Start the daemon

```bash
logoscore -D -m ./modules > logs.txt &
```

```bash
sleep 3
```

### 4.2 Load the providers

```bash
logoscore load-module temperature_module
logoscore load-module request_counter_module
```

### 4.3 Load the scraper

```bash
logoscore load-module openmetrics
```

### 4.4 Start the OpenMetrics server

`start` takes one config JSON: the port, and the list of modules to
scrape. Passed as a single argument so the JSON reaches the module
intact.

```bash
logoscore call openmetrics start '{"port":9099,"modules":["temperature_module","request_counter_module"]}'
```

```bash
sleep 1
```

### 4.5 Scrape /metrics

The request handler runs on the libmicrohttpd thread; for each configured
module it calls `collectMetrics()` through the bound `metrics_source`
interface, and the SDK marshals that call onto the module's owner thread.
Every series carries a `module="<name>"` label, counters render with the
OpenMetrics `_total` sample, and the document ends with `# EOF`:

```bash
curl http://127.0.0.1:9099/metrics
```

### 4.6 Check /health

```bash
curl http://127.0.0.1:9099/health
```

### 4.7 getInfo reports what it's serving

`getInfo()` returns its status as a JSON string, so `logoscore`
delivers it as an escaped string inside the `result` field.

```bash
logoscore call openmetrics getInfo
```

### 4.8 Stop the server

```bash
logoscore call openmetrics stop
```

### 4.9 Stop the daemon

```bash
logoscore stop
```

```bash
sleep 2
```

### 4.10 Confirm the daemon has stopped

```bash
logoscore status
```

---

## Recap

| Module | Implements | In the scrape |
| ------ | ---------- | ------------- |
| `temperature_module` | `collectMetrics()` → gauge + counter | `room_temperature_celsius{…}`, `sensor_readings_total{…}` |
| `request_counter_module` | `collectMetrics()` → counter + gauge | `http_requests_total{…}`, `inflight_requests{…}` |
| `openmetrics` (this repo) | `start`/`stop`/`getInfo`/`scrape` + HTTP server | aggregates both, serves `/metrics` |

The two counters tick on every scrape (`sensor_readings_total`,
`http_requests_total`), so re-running `curl` shows them climb. Each
`collectMetrics()` call happened on the HTTP server thread and was marshaled
onto the target module's owner thread by the SDK — which is what lets the
scraper stay pure C++. A green run is end-to-end evidence that the
`openmetrics` module on this commit aggregates and serves correctly.
