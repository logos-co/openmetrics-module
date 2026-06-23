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

    std::vector<ModuleSource> mods;
    if (cfg.contains("modules") && cfg["modules"].is_array()) {
        for (const auto& m : cfg["modules"]) {
            if (m.is_string()) {
                // Bare string => structured collectMetrics() source (the default).
                const std::string name = m.get<std::string>();
                if (!name.empty()) mods.push_back({name, /*renderedText=*/false});
            } else if (m.is_object() && m.contains("name") && m["name"].is_string()) {
                // Object => explicit per-module format selector.
                const std::string name = m["name"].get<std::string>();
                if (name.empty()) continue;
                const std::string format = m.value("format", std::string("data"));
                mods.push_back({name, /*renderedText=*/format == "text"});
            }
            // Anything else (malformed entry) is skipped.
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
    LogosMap mods = LogosMap::array();
    for (const auto& src : m_modules) {
        mods.push_back({{"name", src.name}, {"format", src.renderedText ? "text" : "data"}});
    }
    info["modules"] = std::move(mods);
    return info.dump();
}

std::string OpenmetricsImpl::scrape() {
    // Snapshot the configured module list without holding the lock across IPC.
    std::vector<ModuleSource> mods;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        mods = m_modules;
    }

    std::vector<openmetrics::ModuleMetrics> collected;
    collected.reserve(mods.size());
    for (const auto& src : mods) {
        // Bind the metrics_source interface to this module name and collect
        // through the typed bound wrapper. The SDK marshals the IPC onto the
        // main thread. A module that doesn't implement the called method (or
        // errors) yields an empty payload, which the formatter skips — one bad
        // module never breaks a scrape.
        LogosMap payload;
        if (src.renderedText) {
            // The module hands back an already-rendered OpenMetrics document;
            // parse it back into the {"metrics":[...]} shape so it merges — and
            // picks up the module="<name>" label — exactly like a structured
            // source.
            std::string text = modules().bind_metrics_source(src.name).collectOpenMetricsText();
            payload = openmetrics::parseOpenMetricsText(text);
        } else {
            payload = modules().bind_metrics_source(src.name).collectMetrics();
        }
        collected.push_back({src.name, std::move(payload)});
    }

    return openmetrics::toOpenMetricsText(collected);
}
