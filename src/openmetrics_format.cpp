#include "openmetrics_format.h"

#include <initializer_list>
#include <map>
#include <sstream>
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
// The full set per the spec: gauge, counter, histogram, gaugehistogram, summary,
// info, stateset, unknown.
std::string sanitizeType(const LogosMap& metric) {
    if (!metric.contains("type") || !metric["type"].is_string()) return "unknown";
    const std::string t = metric["type"].get<std::string>();
    if (t == "counter" || t == "gauge" || t == "histogram" || t == "gaugehistogram" ||
        t == "summary" || t == "info" || t == "stateset" || t == "unknown") {
        return t;
    }
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

bool endsWithSuffix(const std::string& name, const std::string& suf) {
    return name.size() > suf.size() &&
           name.compare(name.size() - suf.size(), suf.size(), suf) == 0;
}

// Map a sample's metric name to its (family, sampleName). The HELP and TYPE lines
// are emitted once per *family*; multi-sample types (histogram, summary, …) keep
// several distinct sample names under one family. The mapping mirrors the
// OpenMetrics sample-suffix conventions per type:
//   counter        -> <family>_total (+ optional _created)
//   histogram      -> <family>_bucket / _sum / _count (+ _created)
//   gaugehistogram -> <family>_bucket / _gsum / _gcount
//   summary        -> <family> (quantile samples) / _sum / _count (+ _created)
//   info           -> <family>_info
//   gauge/stateset/unknown -> the sample name is the family
// For a structured counter/info given the bare family name, the suffix is added.
std::pair<std::string, std::string> familyAndSample(const std::string& name,
                                                     const std::string& type) {
    auto stripFirst = [&](std::initializer_list<const char*> suffixes,
                          std::string& family) -> bool {
        for (const char* s : suffixes) {
            const std::string suf(s);
            if (endsWithSuffix(name, suf)) { family = name.substr(0, name.size() - suf.size()); return true; }
        }
        return false;
    };

    std::string family;
    if (type == "counter") {
        if (stripFirst({"_total", "_created"}, family)) return {family, name};
        return {name, name + "_total"};  // structured `foo` exposes `foo_total`
    }
    if (type == "histogram") {
        if (stripFirst({"_bucket", "_sum", "_count", "_created"}, family)) return {family, name};
        return {name, name};
    }
    if (type == "gaugehistogram") {
        if (stripFirst({"_bucket", "_gsum", "_gcount"}, family)) return {family, name};
        return {name, name};
    }
    if (type == "summary") {
        if (stripFirst({"_sum", "_count", "_created"}, family)) return {family, name};
        return {name, name};  // quantile samples carry the bare family name
    }
    if (type == "info") {
        if (endsWithSuffix(name, "_info")) return {name.substr(0, name.size() - 5), name};
        return {name, name + "_info"};  // structured `foo` exposes `foo_info`
    }
    // gauge, stateset, unknown: the sample name is the family name.
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

namespace {

// Reverse of escapeHelp/escapeLabel: turn the OpenMetrics escapes `\\`, `\"`, and
// `\n` back into their literal characters, so a re-render re-escapes cleanly.
std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[i + 1];
            if (n == '\\')      { out += '\\'; ++i; }
            else if (n == '"')  { out += '"';  ++i; }
            else if (n == 'n')  { out += '\n'; ++i; }
            else                { out += s[i]; }  // unknown escape: keep the backslash
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string trimSpaces(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// A `# TYPE`/`# HELP` line. `hashPos` indexes the '#'. Records into the maps;
// other metadata (`# UNIT`, `# EOF`) and bare comments are ignored.
void parseMetaLine(const std::string& line, size_t hashPos,
                   std::map<std::string, std::string>& typeByFamily,
                   std::map<std::string, std::string>& helpByFamily) {
    std::istringstream ss(line.substr(hashPos + 1));
    std::string keyword;
    ss >> keyword;
    if (keyword == "TYPE") {
        std::string family, type;
        ss >> family >> type;
        if (!family.empty() && !type.empty()) typeByFamily[family] = type;
    } else if (keyword == "HELP") {
        std::string family;
        ss >> family;
        if (family.empty()) return;
        std::string rest;
        std::getline(ss, rest);                 // the remainder is the help text
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        helpByFamily[family] = unescape(rest);
    }
}

// Parse a `{k="v",...}` label set starting at `open` (the '{'). Fills `labels`
// (escape-aware, dropping any `module` label) and sets `closeOut` to the index
// just past the matching '}'. Returns false on a malformed set.
bool parseLabelSet(const std::string& line, size_t open,
                   LogosMap& labels, size_t& closeOut) {
    size_t i = open + 1;  // past '{'
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == ',')) ++i;
        if (i < line.size() && line[i] == '}') { closeOut = i + 1; return true; }
        if (i >= line.size()) return false;

        const size_t keyStart = i;
        while (i < line.size() && line[i] != '=' && line[i] != '}') ++i;
        if (i >= line.size() || line[i] != '=') return false;
        const std::string key = trimSpaces(line.substr(keyStart, i - keyStart));
        ++i;  // past '='

        if (i >= line.size() || line[i] != '"') return false;
        ++i;  // past opening quote

        std::string val;
        bool closed = false;
        while (i < line.size()) {
            const char c = line[i];
            if (c == '\\' && i + 1 < line.size()) {
                const char n = line[i + 1];
                if (n == '\\')      val += '\\';
                else if (n == '"')  val += '"';
                else if (n == 'n')  val += '\n';
                else              { val += '\\'; val += n; }
                i += 2;
            } else if (c == '"') {
                ++i;  // past closing quote
                closed = true;
                break;
            } else {
                val += c;
                ++i;
            }
        }
        if (!closed) return false;

        // Drop a pre-existing `module` label: the renderer injects the
        // authoritative one, and OpenMetrics forbids a duplicate label name.
        if (!key.empty() && key != "module") labels[key] = val;
    }
    return false;  // ran off the end without a closing '}'
}

// Map a sample name to its declared family + type. Gauges/statesets/summary
// quantiles match the family name directly; the multi-sample suffixes (`_total`,
// `_bucket`, `_sum`, `_count`, `_gsum`, `_gcount`, `_info`, `_created`) resolve to
// the family that declared the TYPE. Falls back to `unknown`/own-name.
std::pair<std::string, std::string> familyForSample(
    const std::string& name, const std::map<std::string, std::string>& typeByFamily) {
    auto direct = typeByFamily.find(name);
    if (direct != typeByFamily.end()) return {name, direct->second};

    static const char* const kSuffixes[] = {
        "_total", "_count", "_sum", "_bucket", "_gcount", "_gsum", "_info", "_created"};
    for (const char* suf : kSuffixes) {
        if (endsWithSuffix(name, suf)) {
            const std::string base = name.substr(0, name.size() - std::string(suf).size());
            auto it = typeByFamily.find(base);
            if (it != typeByFamily.end()) return {base, it->second};
        }
    }
    return {name, "unknown"};
}

// Parse one sample line `name[{labels}] value [timestamp] [# exemplar]` into a
// metric object. Returns false if it isn't a usable sample.
bool parseSampleLine(const std::string& line, size_t start,
                     const std::map<std::string, std::string>& typeByFamily,
                     const std::map<std::string, std::string>& helpByFamily,
                     LogosMap& outMetric) {
    size_t i = start;
    const size_t nameStart = i;
    while (i < line.size() && line[i] != '{' && line[i] != ' ' && line[i] != '\t') ++i;
    const std::string name = line.substr(nameStart, i - nameStart);
    if (name.empty()) return false;

    LogosMap labels = LogosMap::object();
    if (i < line.size() && line[i] == '{') {
        size_t close = 0;
        if (!parseLabelSet(line, i, labels, close)) return false;
        i = close;
    }

    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size()) return false;  // no value
    const size_t valStart = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    const std::string value = line.substr(valStart, i - valStart);
    if (value.empty()) return false;

    const auto [family, type] = familyForSample(name, typeByFamily);

    outMetric = LogosMap::object();
    outMetric["name"] = name;
    outMetric["type"] = type;
    auto h = helpByFamily.find(family);
    if (h != helpByFamily.end() && !h->second.empty()) outMetric["help"] = h->second;
    // Keep the numeric token verbatim — renderValue() passes numeric strings
    // through unchanged, so values round-trip exactly (`21`→`21`, `3.14`→`3.14`).
    outMetric["value"] = value;
    if (!labels.empty()) outMetric["labels"] = std::move(labels);
    return true;
}

}  // namespace

LogosMap parseOpenMetricsText(const std::string& text) {
    std::map<std::string, std::string> typeByFamily;  // family -> type, from `# TYPE`
    std::map<std::string, std::string> helpByFamily;  // family -> help, from `# HELP`
    LogosMap metrics = LogosMap::array();

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF

        const size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos) continue;  // blank line

        if (line[i] == '#') {
            parseMetaLine(line, i, typeByFamily, helpByFamily);
            continue;
        }

        LogosMap metric;
        if (parseSampleLine(line, i, typeByFamily, helpByFamily, metric)) {
            metrics.push_back(std::move(metric));
        }
    }

    return {{"metrics", std::move(metrics)}};
}

}  // namespace openmetrics
