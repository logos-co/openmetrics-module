#pragma once

// metrics_demo — a tiny example module that implements the `collectMetrics()`
// contract (interfaces/metrics_source.h in the openmetrics module) so the
// openmetrics scraper has something to scrape. Pure universal C++.

#include <logos_json.h>  // LogosMap (nlohmann::json alias)

class MetricsDemoImpl {
public:
    // Return a {"metrics": [...]} payload of openmetrics-like fields.
    LogosMap collectMetrics();
};
