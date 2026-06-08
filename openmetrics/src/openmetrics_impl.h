#pragma once

// openmetrics — a universal (pure-C++) Logos module that serves an OpenMetrics
// `/metrics` endpoint by scraping a configured set of modules.
//
// It declares an interface dependency on `metrics_source` (the collectMetrics()
// contract, see interfaces/metrics_source.h) rather than depending on any
// concrete module. It binds that interface to each operator-configured module
// name and calls collectMetrics() through the typed bound wrapper.
//
// No Qt here: the libmicrohttpd server runs on its own thread and calls
// scrape() directly, which performs inter-module IPC. The SDK transparently
// marshals that IPC onto the module's main/event-loop thread (where Qt Remote
// Objects replicas live), so this module stays pure C++.
//
// The impl header stays free of the generated logos_sdk.h and of the
// libmicrohttpd headers — the generator parses it and expects plain C++. Those
// live in the .cpp.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <logos_module_context.h>  // LogosModuleContext base (gives modules())

class OpenmetricsImpl : public LogosModuleContext {
public:
    OpenmetricsImpl() = default;
    ~OpenmetricsImpl();

    // Parse the config JSON ({"port": <int>, "modules": ["<name>", ...]}) and
    // start the HTTP server. Returns 1 on success, 0 on failure (bad config /
    // already running / bind error).
    int64_t start(const std::string& configJson);

    // Stop the HTTP server. Returns 1 if it was running and is now stopped, 0
    // if it was not running.
    int64_t stop();

    // Current state as JSON: {"running": bool, "port": <int>, "modules": [...]}.
    std::string getInfo();

    // Collect from every configured module and render the OpenMetrics document.
    // Performs inter-module IPC (SDK-marshaled to the main thread). Called by
    // the HTTP `/metrics` handler, and exposed as a module method for debugging
    // (`logoscore call openmetrics scrape`).
    std::string scrape();

private:
    std::mutex m_mutex;
    void* m_daemon = nullptr;  // struct MHD_Daemon* (opaque here to keep MHD out of the header)
    int m_port = 0;
    std::vector<std::string> m_modules;
};
