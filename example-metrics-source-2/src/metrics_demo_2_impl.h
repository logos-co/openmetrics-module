#pragma once

// metrics_demo_2 — a second example module implementing the collectMetrics()
// contract, so the openmetrics scraper can aggregate across multiple modules.
// Pure universal C++.

#include <logos_json.h>  // LogosMap (nlohmann::json alias)

class MetricsDemo2Impl {
public:
    // Return a {"metrics": [...]} payload of openmetrics-like fields.
    LogosMap collectMetrics();
};
