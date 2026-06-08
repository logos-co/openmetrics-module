#include "openmetrics_impl.h"

#include <cstdint>
#include <utility>

#include <microhttpd.h>

#include "openmetrics_format.h"

// Generated at build time by logos-cpp-generator. Defines `LogosModules` with
// the `bind_metrics_source(moduleName)` factory (because metadata.json declares
// an interface_dependency on `metrics_source`). Included only in the .cpp so the
// impl header the generator parses stays free of codegen types.
#include "logos_sdk.h"

namespace {

constexpr const char* kOpenMetricsContentType =
    "application/openmetrics-text; version=1.0.0; charset=utf-8";

MHD_Daemon* asDaemon(void* p) { return static_cast<MHD_Daemon*>(p); }

// libmicrohttpd access handler. `cls` is the OpenmetricsImpl*. Runs on an MHD
// worker thread; the inter-module IPC inside scrape() is marshaled onto the
// module's main thread by the SDK.
MHD_Result onRequest(void* cls, struct MHD_Connection* connection, const char* url,
                     const char* method, const char* /*version*/,
                     const char* /*upload_data*/, size_t* /*upload_data_size*/,
                     void** /*req_cls*/) {
    auto* self = static_cast<OpenmetricsImpl*>(cls);
    const std::string path = url ? url : "/";
    const bool isGet = method && std::string(method) == "GET";

    std::string body;
    const char* contentType = "text/plain; charset=utf-8";
    unsigned int status = MHD_HTTP_OK;

    if (isGet && path == "/metrics") {
        body = self->scrape();
        contentType = kOpenMetricsContentType;
    } else if (isGet && path == "/health") {
        body = "ok\n";
    } else {
        body = "not found\n";
        status = MHD_HTTP_NOT_FOUND;
    }

    MHD_Response* response = MHD_create_response_from_buffer(
        body.size(), const_cast<char*>(body.data()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", contentType);
    MHD_Result ret = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return ret;
}

}  // namespace

OpenmetricsImpl::~OpenmetricsImpl() {
    stop();
}

int64_t OpenmetricsImpl::start(const std::string& configJson) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_daemon) return 0;  // already running

    LogosMap cfg;
    try {
        cfg = LogosMap::parse(configJson);
    } catch (...) {
        return 0;
    }

    const int port = cfg.value("port", 0);
    if (port <= 0 || port > 65535) return 0;

    std::vector<std::string> mods;
    if (cfg.contains("modules") && cfg["modules"].is_array()) {
        for (const auto& m : cfg["modules"]) {
            if (m.is_string()) mods.push_back(m.get<std::string>());
        }
    }

    MHD_Daemon* daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD, static_cast<uint16_t>(port),
        /*apc=*/nullptr, /*apc_cls=*/nullptr,
        &onRequest, this,
        MHD_OPTION_END);
    if (!daemon) return 0;

    m_daemon = daemon;
    m_port = port;
    m_modules = std::move(mods);
    return 1;
}

int64_t OpenmetricsImpl::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_daemon) return 0;
    MHD_stop_daemon(asDaemon(m_daemon));
    m_daemon = nullptr;
    m_port = 0;
    m_modules.clear();
    return 1;
}

std::string OpenmetricsImpl::getInfo() {
    std::lock_guard<std::mutex> lock(m_mutex);
    LogosMap info;
    info["running"] = (m_daemon != nullptr);
    info["port"] = m_port;
    info["modules"] = m_modules;
    return info.dump();
}

std::string OpenmetricsImpl::scrape() {
    // Snapshot the configured module list without holding the lock across IPC.
    std::vector<std::string> mods;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        mods = m_modules;
    }

    std::vector<openmetrics::ModuleMetrics> collected;
    collected.reserve(mods.size());
    for (const auto& name : mods) {
        // Bind the metrics_source interface to this module name and call its
        // collectMetrics() through the typed wrapper. The SDK marshals the IPC
        // onto the main thread. A module that doesn't implement it (or errors)
        // yields an empty LogosMap, which the formatter skips — one bad module
        // never breaks a scrape.
        LogosMap payload = modules().bind_metrics_source(name).collectMetrics();
        collected.push_back({name, std::move(payload)});
    }

    return openmetrics::toOpenMetricsText(collected);
}
