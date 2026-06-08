#include "openmetrics_format.h"

#include <map>
#include <utility>

namespace openmetrics {
namespace {

// Escape a `# HELP` description: backslash and newline only.
std::string escapeHelp(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// Escape a label value: backslash, double-quote, and newline.
std::string escapeLabel(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// Restrict to the OpenMetrics-known type set; unknown/missing -> "unknown".
std::string sanitizeType(const LogosMap& metric) {
    if (!metric.contains("type") || !metric["type"].is_string()) return "unknown";
    const std::string t = metric["type"].get<std::string>();
    if (t == "counter" || t == "gauge" || t == "histogram" || t == "summary") return t;
    return "unknown";
}

// Render a metric value as an OpenMetrics number. Bools map to 1/0; numeric
// strings pass through. Returns false if the value can't be rendered.
bool renderValue(const LogosMap& metric, std::string& out) {
    if (!metric.contains("value")) return false;
    const LogosMap& v = metric["value"];
    if (v.is_number_integer())  { out = std::to_string(v.get<long long>()); return true; }
    if (v.is_number_unsigned()) { out = std::to_string(v.get<unsigned long long>()); return true; }
    if (v.is_number_float())    { out = v.dump(); return true; }
    if (v.is_boolean())         { out = v.get<bool>() ? "1" : "0"; return true; }
    if (v.is_string()) {
        // Accept only numeric strings.
        const std::string s = v.get<std::string>();
        try { (void)std::stod(s); out = s; return true; } catch (...) { return false; }
    }
    return false;
}

// OpenMetrics counters expose their total sample as `<family>_total`; the HELP
// and TYPE lines use the family name (without `_total`).
std::pair<std::string, std::string> familyAndSample(const std::string& name,
                                                     const std::string& type) {
    const std::string suffix = "_total";
    if (type == "counter") {
        if (name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return {name.substr(0, name.size() - suffix.size()), name};
        }
        return {name, name + suffix};
    }
    return {name, name};
}

struct Family {
    std::string type;
    std::string help;
    std::vector<std::string> samples;
};

std::string renderLabels(const std::string& module, const LogosMap& metric) {
    std::string out = "module=\"" + escapeLabel(module) + "\"";
    if (metric.contains("labels") && metric["labels"].is_object()) {
        for (auto it = metric["labels"].begin(); it != metric["labels"].end(); ++it) {
            std::string val;
            if (it.value().is_string()) val = it.value().get<std::string>();
            else val = it.value().dump();
            out += "," + it.key() + "=\"" + escapeLabel(val) + "\"";
        }
    }
    return out;
}

}  // namespace

std::string toOpenMetricsText(const std::vector<ModuleMetrics>& collected) {
    // Grouped by family name; std::map keeps output deterministic.
    std::map<std::string, Family> families;

    for (const auto& mm : collected) {
        if (!mm.payload.contains("metrics") || !mm.payload["metrics"].is_array()) continue;

        for (const auto& metric : mm.payload["metrics"]) {
            if (!metric.is_object()) continue;
            if (!metric.contains("name") || !metric["name"].is_string()) continue;
            const std::string name = metric["name"].get<std::string>();
            if (name.empty()) continue;

            std::string value;
            if (!renderValue(metric, value)) continue;

            const std::string type = sanitizeType(metric);
            const auto [family, sampleName] = familyAndSample(name, type);

            const std::string help =
                (metric.contains("help") && metric["help"].is_string())
                    ? metric["help"].get<std::string>()
                    : std::string();

            std::string sample = sampleName + "{" + renderLabels(mm.module, metric) + "} " + value;

            auto& fam = families[family];
            if (fam.samples.empty()) {  // first time we see this family
                fam.type = type;
                fam.help = help;
            }
            fam.samples.push_back(std::move(sample));
        }
    }

    std::string out;
    for (const auto& [name, fam] : families) {
        if (!fam.help.empty()) {
            out += "# HELP " + name + " " + escapeHelp(fam.help) + "\n";
        }
        out += "# TYPE " + name + " " + fam.type + "\n";
        for (const auto& sample : fam.samples) {
            out += sample;
            out += "\n";
        }
    }
    out += "# EOF\n";  // OpenMetrics requires the document to end with this.
    return out;
}

}  // namespace openmetrics
