// Unit tests for the OpenMetrics text <-> LogosMap layer (openmetrics_format.cpp).
//
// These exercise the parse + render round-trip across every metric type the
// OpenMetrics standard defines — gauge, counter, histogram, gaugehistogram,
// summary, info, stateset, unknown — plus the exposition-format details: label
// and HELP escaping, the full numeric value set (ints, floats, exponents,
// +Inf/-Inf/NaN), timestamps, exemplars, comments, # UNIT lines, CRLF, and the
// module="<name>" label the scraper injects when it merges a rendered-text
// source. parseOpenMetricsText is what turns a collectOpenMetricsText() payload
// into the same shape collectMetrics() returns, so a text source merges into a
// scrape exactly like a structured one; toOpenMetricsText renders the merged
// result.

#include <logos_test.h>

#include <string>
#include <vector>

#include "openmetrics_format.h"

namespace {

// Parse a rendered document and render it back under one module name — the exact
// path a "format": "text" source takes through a scrape.
std::string roundtrip(const std::string& doc, const std::string& module = "m") {
    std::vector<openmetrics::ModuleMetrics> collected{
        {module, openmetrics::parseOpenMetricsText(doc)}};
    return openmetrics::toOpenMetricsText(collected);
}

size_t countOccurrences(const std::string& hay, const std::string& needle) {
    size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
    return n;
}

bool absent(const std::string& hay, const std::string& needle) {
    return hay.find(needle) == std::string::npos;
}

}  // namespace

// ── gauge ────────────────────────────────────────────────────────────────────
LOGOS_TEST(gauge_roundtrips_with_label) {
    auto out = roundtrip(
        "# HELP room_temp Current temperature\n"
        "# TYPE room_temp gauge\n"
        "room_temp{sensor=\"core\"} 21\n"
        "# EOF\n");
    LOGOS_ASSERT_CONTAINS(out, "# HELP room_temp Current temperature\n");
    LOGOS_ASSERT_CONTAINS(out, "# TYPE room_temp gauge\n");
    LOGOS_ASSERT_CONTAINS(out, "room_temp{module=\"m\",sensor=\"core\"} 21\n");
    LOGOS_ASSERT_CONTAINS(out, "# EOF\n");
}

// ── counter: _total (+ _created companion) group under one family ────────────
LOGOS_TEST(counter_total_and_created_group_under_one_family) {
    auto out = roundtrip(
        "# TYPE http_requests counter\n"
        "# HELP http_requests Total requests\n"
        "http_requests_total{method=\"GET\"} 7\n"
        "http_requests_created 1700000000.0\n"
        "# EOF\n");
    LOGOS_ASSERT_EQ(countOccurrences(out, "# TYPE http_requests counter"), size_t(1));
    LOGOS_ASSERT_EQ(countOccurrences(out, "# HELP http_requests Total requests"), size_t(1));
    LOGOS_ASSERT_CONTAINS(out, "http_requests_total{module=\"m\",method=\"GET\"} 7\n");
    LOGOS_ASSERT_CONTAINS(out, "http_requests_created{module=\"m\"} 1700000000.0\n");
    LOGOS_ASSERT_TRUE(absent(out, "http_requests_total_total"));  // no double suffix
}

// A structured counter given the bare family name still exposes <family>_total.
LOGOS_TEST(structured_counter_bare_name_gets_total_suffix) {
    // No _total in the sample name, but TYPE says counter — renderer adds _total.
    auto out = roundtrip(
        "# TYPE jobs counter\n"
        "jobs 3\n"
        "# EOF\n");
    LOGOS_ASSERT_CONTAINS(out, "# TYPE jobs counter\n");
    LOGOS_ASSERT_CONTAINS(out, "jobs_total{module=\"m\"} 3\n");
}

// ── histogram: bucket/sum/count under ONE family, order + +Inf preserved ─────
LOGOS_TEST(histogram_groups_components_under_one_family) {
    auto out = roundtrip(
        "# TYPE request_latency_seconds histogram\n"
        "# HELP request_latency_seconds Latency\n"
        "request_latency_seconds_bucket{le=\"0.1\"} 1\n"
        "request_latency_seconds_bucket{le=\"0.5\"} 2\n"
        "request_latency_seconds_bucket{le=\"+Inf\"} 3\n"
        "request_latency_seconds_sum 1.23\n"
        "request_latency_seconds_count 3\n"
        "# EOF\n");
    // Exactly one HELP/TYPE for the whole family — no per-suffix pseudo-families.
    LOGOS_ASSERT_EQ(countOccurrences(out, "# TYPE request_latency_seconds histogram"), size_t(1));
    LOGOS_ASSERT_EQ(countOccurrences(out, "# HELP request_latency_seconds Latency"), size_t(1));
    LOGOS_ASSERT_TRUE(absent(out, "# TYPE request_latency_seconds_bucket"));
    LOGOS_ASSERT_TRUE(absent(out, "# TYPE request_latency_seconds_sum"));
    LOGOS_ASSERT_TRUE(absent(out, "# TYPE request_latency_seconds_count"));
    // Samples present with injected module label; +Inf bucket preserved.
    LOGOS_ASSERT_CONTAINS(out, "request_latency_seconds_bucket{module=\"m\",le=\"0.1\"} 1\n");
    LOGOS_ASSERT_CONTAINS(out, "request_latency_seconds_bucket{module=\"m\",le=\"+Inf\"} 3\n");
    LOGOS_ASSERT_CONTAINS(out, "request_latency_seconds_sum{module=\"m\"} 1.23\n");
    LOGOS_ASSERT_CONTAINS(out, "request_latency_seconds_count{module=\"m\"} 3\n");
    // Bucket order preserved: 0.1 before 0.5 before +Inf.
    const size_t p1 = out.find("le=\"0.1\"");
    const size_t p2 = out.find("le=\"0.5\"");
    const size_t p3 = out.find("le=\"+Inf\"");
    LOGOS_ASSERT_TRUE(p1 < p2 && p2 < p3);
}

// ── gaugehistogram: _bucket/_gsum/_gcount under one family ───────────────────
LOGOS_TEST(gaugehistogram_groups_components) {
    auto out = roundtrip(
        "# TYPE temp_celsius gaugehistogram\n"
        "temp_celsius_bucket{le=\"0\"} 0\n"
        "temp_celsius_bucket{le=\"+Inf\"} 7\n"
        "temp_celsius_gsum 9.5\n"
        "temp_celsius_gcount 7\n"
        "# EOF\n");
    LOGOS_ASSERT_EQ(countOccurrences(out, "# TYPE temp_celsius gaugehistogram"), size_t(1));
    LOGOS_ASSERT_CONTAINS(out, "temp_celsius_bucket{module=\"m\",le=\"+Inf\"} 7\n");
    LOGOS_ASSERT_CONTAINS(out, "temp_celsius_gsum{module=\"m\"} 9.5\n");
    LOGOS_ASSERT_CONTAINS(out, "temp_celsius_gcount{module=\"m\"} 7\n");
    LOGOS_ASSERT_TRUE(absent(out, "# TYPE temp_celsius_gsum"));
}

// ── summary: quantile samples + _sum/_count under one family ─────────────────
LOGOS_TEST(summary_groups_quantiles_with_sum_and_count) {
    auto out = roundtrip(
        "# TYPE rpc_duration_seconds summary\n"
        "rpc_duration_seconds{quantile=\"0.5\"} 0.2\n"
        "rpc_duration_seconds{quantile=\"0.99\"} 0.9\n"
        "rpc_duration_seconds_sum 12\n"
        "rpc_duration_seconds_count 100\n"
        "# EOF\n");
    LOGOS_ASSERT_EQ(countOccurrences(out, "# TYPE rpc_duration_seconds summary"), size_t(1));
    LOGOS_ASSERT_CONTAINS(out, "rpc_duration_seconds{module=\"m\",quantile=\"0.5\"} 0.2\n");
    LOGOS_ASSERT_CONTAINS(out, "rpc_duration_seconds{module=\"m\",quantile=\"0.99\"} 0.9\n");
    LOGOS_ASSERT_CONTAINS(out, "rpc_duration_seconds_sum{module=\"m\"} 12\n");
    LOGOS_ASSERT_CONTAINS(out, "rpc_duration_seconds_count{module=\"m\"} 100\n");
}

// ── info: <family>_info sample under one family ──────────────────────────────
LOGOS_TEST(info_groups_under_family) {
    auto out = roundtrip(
        "# TYPE build info\n"
        "build_info{version=\"1.2.3\",branch=\"main\"} 1\n"
        "# EOF\n");
    LOGOS_ASSERT_CONTAINS(out, "# TYPE build info\n");
    // Labels render alphabetically after the injected module label.
    LOGOS_ASSERT_CONTAINS(out, "build_info{module=\"m\",branch=\"main\",version=\"1.2.3\"} 1\n");
}

// ── stateset: one sample per state under one family ──────────────────────────
LOGOS_TEST(stateset_groups_states) {
    auto out = roundtrip(
        "# TYPE feature_flags stateset\n"
        "feature_flags{feature_flags=\"alpha\"} 1\n"
        "feature_flags{feature_flags=\"beta\"} 0\n"
        "# EOF\n");
    LOGOS_ASSERT_EQ(countOccurrences(out, "# TYPE feature_flags stateset"), size_t(1));
    LOGOS_ASSERT_CONTAINS(out, "feature_flags{module=\"m\",feature_flags=\"alpha\"} 1\n");
    LOGOS_ASSERT_CONTAINS(out, "feature_flags{module=\"m\",feature_flags=\"beta\"} 0\n");
}

// ── unknown: a bare sample with no # TYPE defaults to unknown ─────────────────
LOGOS_TEST(untyped_sample_defaults_to_unknown) {
    auto out = roundtrip(
        "mystery_metric 42\n"
        "# EOF\n");
    LOGOS_ASSERT_CONTAINS(out, "# TYPE mystery_metric unknown\n");
    LOGOS_ASSERT_CONTAINS(out, "mystery_metric{module=\"m\"} 42\n");
}

// ── module label injection + dropping a pre-existing module label ────────────
LOGOS_TEST(injects_module_label_and_drops_preexisting) {
    auto out = roundtrip(
        "# TYPE up gauge\n"
        "up{module=\"stale\",instance=\"a\"} 1\n"
        "# EOF\n",
        "real_module");
    LOGOS_ASSERT_CONTAINS(out, "up{module=\"real_module\",instance=\"a\"} 1\n");
    LOGOS_ASSERT_TRUE(absent(out, "stale"));                 // old module value gone
    LOGOS_ASSERT_EQ(countOccurrences(out, "module="), size_t(1));  // no duplicate label
}

// ── escaping: backslash / quote / newline in label values and HELP ───────────
LOGOS_TEST(escaping_roundtrips_in_labels_and_help) {
    // The document contains escaped backslash (\\), newline (\n), and — in the
    // label value — an escaped quote (\"). All must survive parse + re-render.
    auto out = roundtrip(
        "# HELP msgs back \\\\ and nl \\n done\n"
        "# TYPE msgs gauge\n"
        "msgs{text=\"q \\\" b \\\\ nl \\n\"} 1\n"
        "# EOF\n");
    LOGOS_ASSERT_CONTAINS(out, "# HELP msgs back \\\\ and nl \\n done\n");
    LOGOS_ASSERT_CONTAINS(out, "msgs{module=\"m\",text=\"q \\\" b \\\\ nl \\n\"} 1\n");
}

// ── value formats: int, float, exponent, negatives, +Inf/-Inf/NaN ────────────
LOGOS_TEST(value_formats_pass_through_verbatim) {
    auto out = roundtrip(
        "v_int 42\n"
        "v_float 3.14\n"
        "v_exp 1.5e-9\n"
        "v_neg -2\n"
        "v_pos_inf{kind=\"a\"} +Inf\n"
        "v_neg_inf{kind=\"b\"} -Inf\n"
        "v_nan NaN\n"
        "# EOF\n");
    LOGOS_ASSERT_CONTAINS(out, "v_int{module=\"m\"} 42\n");
    LOGOS_ASSERT_CONTAINS(out, "v_float{module=\"m\"} 3.14\n");
    LOGOS_ASSERT_CONTAINS(out, "v_exp{module=\"m\"} 1.5e-9\n");
    LOGOS_ASSERT_CONTAINS(out, "v_neg{module=\"m\"} -2\n");
    LOGOS_ASSERT_CONTAINS(out, "v_pos_inf{module=\"m\",kind=\"a\"} +Inf\n");
    LOGOS_ASSERT_CONTAINS(out, "v_neg_inf{module=\"m\",kind=\"b\"} -Inf\n");
    LOGOS_ASSERT_CONTAINS(out, "v_nan{module=\"m\"} NaN\n");
}

// ── timestamps and exemplars are ignored; the value is still captured ─────────
LOGOS_TEST(timestamps_and_exemplars_are_ignored) {
    auto out = roundtrip(
        "# TYPE hits counter\n"
        "hits_total 5 1700000000000\n"                        // trailing timestamp
        "# TYPE lat histogram\n"
        "lat_bucket{le=\"1\"} 3 # {trace_id=\"abc\"} 0.5\n"   // exemplar after value
        "# EOF\n");
    LOGOS_ASSERT_CONTAINS(out, "hits_total{module=\"m\"} 5\n");
    LOGOS_ASSERT_CONTAINS(out, "lat_bucket{module=\"m\",le=\"1\"} 3\n");
    LOGOS_ASSERT_TRUE(absent(out, "1700000000000"));
    LOGOS_ASSERT_TRUE(absent(out, "trace_id"));
}

// ── comments, # UNIT, blank lines, and CRLF line endings are tolerated ────────
LOGOS_TEST(comments_unit_blanks_and_crlf_are_tolerated) {
    auto out = roundtrip(
        "# a free-form comment\n"
        "\n"
        "# UNIT temp_seconds seconds\r\n"
        "# TYPE temp_seconds gauge\r\n"
        "temp_seconds 5\r\n"
        "# EOF\r\n");
    LOGOS_ASSERT_CONTAINS(out, "# TYPE temp_seconds gauge\n");
    LOGOS_ASSERT_CONTAINS(out, "temp_seconds{module=\"m\"} 5\n");
    LOGOS_ASSERT_TRUE(absent(out, "free-form comment"));
    LOGOS_ASSERT_TRUE(absent(out, "UNIT"));
}

// ── multiple modules merge into one family with one HELP/TYPE ─────────────────
LOGOS_TEST(multiple_modules_merge_into_one_family) {
    std::vector<openmetrics::ModuleMetrics> collected{
        {"mod_a", openmetrics::parseOpenMetricsText(
                      "# TYPE up gauge\n# HELP up Is it up\nup 1\n# EOF\n")},
        {"mod_b", openmetrics::parseOpenMetricsText(
                      "# TYPE up gauge\n# HELP up Is it up\nup 0\n# EOF\n")},
    };
    auto out = openmetrics::toOpenMetricsText(collected);
    LOGOS_ASSERT_EQ(countOccurrences(out, "# TYPE up gauge"), size_t(1));
    LOGOS_ASSERT_EQ(countOccurrences(out, "# HELP up Is it up"), size_t(1));
    LOGOS_ASSERT_CONTAINS(out, "up{module=\"mod_a\"} 1\n");
    LOGOS_ASSERT_CONTAINS(out, "up{module=\"mod_b\"} 0\n");
}

// ── a second round-trip is a fixed point (stable output) ─────────────────────
LOGOS_TEST(roundtrip_is_idempotent) {
    const std::string doc =
        "# TYPE lat histogram\n"
        "# HELP lat Latency\n"
        "lat_bucket{le=\"0.5\"} 2\n"
        "lat_bucket{le=\"+Inf\"} 3\n"
        "lat_sum 1.5\n"
        "lat_count 3\n"
        "# EOF\n";
    const std::string once = roundtrip(doc);
    const std::string twice = roundtrip(once);  // re-parse + re-render the output
    LOGOS_ASSERT_EQ(once, twice);
}

// ── malformed lines are skipped; good samples around them survive ─────────────
LOGOS_TEST(malformed_lines_are_skipped) {
    auto out = roundtrip(
        "# TYPE ok gauge\n"
        "ok 1\n"
        "bad_no_value\n"               // a name with no value
        "unterminated{label=\"x 2\n"   // label set never closes its quote
        "# EOF\n");
    LOGOS_ASSERT_CONTAINS(out, "ok{module=\"m\"} 1\n");
    LOGOS_ASSERT_TRUE(absent(out, "bad_no_value"));
    LOGOS_ASSERT_TRUE(absent(out, "unterminated"));
}

// ── a label value may contain commas and braces inside its quotes ────────────
LOGOS_TEST(label_value_with_braces_and_commas) {
    auto out = roundtrip(
        "# TYPE q gauge\n"
        "q{expr=\"sum(a{b=1,c=2})\"} 1\n"
        "# EOF\n");
    LOGOS_ASSERT_CONTAINS(out, "q{module=\"m\",expr=\"sum(a{b=1,c=2})\"} 1\n");
}

// ── an empty label set `{}` renders with just the injected module label ──────
LOGOS_TEST(empty_label_set_keeps_only_module) {
    auto out = roundtrip(
        "# TYPE e gauge\n"
        "e{} 9\n"
        "# EOF\n");
    LOGOS_ASSERT_CONTAINS(out, "e{module=\"m\"} 9\n");
}

// ── an empty / non-metric document yields a valid (empty) doc, not a crash ────
LOGOS_TEST(empty_document_renders_just_eof) {
    auto out = roundtrip("");
    LOGOS_ASSERT_CONTAINS(out, "# EOF\n");
}
