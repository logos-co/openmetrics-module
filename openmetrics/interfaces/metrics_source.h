#pragma once

// A DEPENDENCY INTERFACE — the contract a module must satisfy to be scraped by
// the openmetrics module. It names NO concrete module: any module whose own API
// includes a matching `collectMetrics()` satisfies it (the superset rule). The
// openmetrics module binds it to operator-chosen module names at runtime via
// modules().bind_metrics_source("some_module").
//
// The Logos generator parses this file and emits a BOUND wrapper class
// `MetricsSource` whose target module name is a runtime ctor argument.
//
// Types are std / LogosMap because the consuming module is interface:
// "universal" — the bound wrapper inherits that api-style.

#include <logos_json.h>            // LogosMap (nlohmann::json alias)
#include <logos_module_context.h>  // defines the `logos_events` token

class IMetricsSource {
public:
    // Return a {"metrics": [ {name, type, help, value, labels?}, ... ]} payload
    // of prometheus/openmetrics-like fields. See the openmetrics README for the
    // field schema.
    LogosMap collectMetrics();
};
