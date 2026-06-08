#pragma once

// OpenMetrics exposition-format rendering. Pure C++ (std + nlohmann::json),
// Qt-free and unit-testable in isolation.
//
// Input: per-module `{"metrics": [...]}` payloads (as returned by each module's
// collectMetrics()). Output: an OpenMetrics text document
// (application/openmetrics-text; version=1.0.0).

#include <string>
#include <vector>

#include <logos_json.h>  // LogosMap (nlohmann::json alias)

namespace openmetrics {

// One module's parsed metrics payload.
struct ModuleMetrics {
    std::string module;
    LogosMap payload;  // expected shape: {"metrics": [ {name,type,help,value,labels?}, ... ]}
};

// Render the collected metrics as an OpenMetrics text document. Samples are
// grouped into families (HELP/TYPE emitted once), every sample carries a
// `module="<name>"` label, counters follow the OpenMetrics `_total` convention,
// and the document is terminated with `# EOF`.
std::string toOpenMetricsText(const std::vector<ModuleMetrics>& collected);

}  // namespace openmetrics
