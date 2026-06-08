#include "metrics_demo_2_impl.h"

#include <atomic>
#include <cstdint>

namespace {
std::atomic<std::int64_t> g_messages{0};
}  // namespace

LogosMap MetricsDemo2Impl::collectMetrics() {
    // Pretend a few messages arrive between scrapes.
    const std::int64_t messages = g_messages.fetch_add(7, std::memory_order_relaxed) + 7;

    LogosMap metrics = LogosMap::array();
    metrics.push_back({
        {"name", "demo2_messages_total"},
        {"type", "counter"},
        {"help", "Messages processed"},
        {"value", messages},
        {"labels", {{"topic", "chat"}}},
    });
    metrics.push_back({
        {"name", "demo2_peers"},
        {"type", "gauge"},
        {"help", "Currently connected peers"},
        {"value", 3},
    });

    return {{"metrics", metrics}};
}
