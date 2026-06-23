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

// Parse an already-rendered OpenMetrics text document into the internal
// `{"metrics": [ {name,type,help,value,labels?}, ... ]}` payload — the same
// shape a module's collectMetrics() returns. This lets a module that exposes
// metrics as rendered text (via collectOpenMetricsText()) merge into a scrape
// exactly like a structured source: feed the result to toOpenMetricsText() and
// it re-renders with the `module="<name>"` label injected and families grouped.
//
// Best-effort and lossless for the common counter/gauge case: HELP/TYPE are read
// from `# TYPE`/`# HELP` lines, label sets are parsed with OpenMetrics escaping,
// and each sample's numeric token is preserved verbatim (so values round-trip
// exactly). Any pre-existing `module` label is dropped so the injected one stays
// authoritative. Unparseable lines, `# UNIT`, `# EOF`, exemplars, and timestamps
// are ignored. Multi-sample families (histogram/summary) re-render flat, matching
// the renderer, which only special-cases the counter `_total` sample.
LogosMap parseOpenMetricsText(const std::string& text);

}  // namespace openmetrics
