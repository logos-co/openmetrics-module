#include "metrics_demo_impl.h"

#include <atomic>
#include <cstdint>

namespace {
// Counts how many times we've been scraped (demonstrates a counter).
std::atomic<std::int64_t> g_scrapes{0};
}  // namespace

LogosMap MetricsDemoImpl::collectMetrics() {
    const std::int64_t scrapes = ++g_scrapes;

    LogosMap metrics = LogosMap::array();
    metrics.push_back({
        {"name", "demo_scrapes_total"},
        {"type", "counter"},
        {"help", "Times collectMetrics was called"},
        {"value", scrapes},
    });
    metrics.push_back({
        {"name", "demo_temperature_celsius"},
        {"type", "gauge"},
        {"help", "A fake temperature reading"},
        {"value", 21},
        {"labels", {{"sensor", "core"}}},
    });

    return {{"metrics", metrics}};
}
