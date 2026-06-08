{
  description = "openmetrics — a Logos module that serves an OpenMetrics /metrics endpoint by scraping modules that implement collectMetrics()";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
  };

  # The repo ships exactly one module: the `openmetrics` scraper. It declares an
  # interface_dependency on `metrics_source` (openmetrics/interfaces/metrics_source.h)
  # — no concrete module dependency — and binds it to operator-chosen module names
  # at runtime.
  #
  # mkLogosModule exposes the usual per-system outputs: `default` (the plugin),
  # `lgx` (a ready-to-install package), `install` (a modules/ tree), etc.
  #
  # Example provider modules and the end-to-end scrape are NOT part of this flake
  # — they live in the literate doc-test under doctests/, which builds this
  # module from the commit under test, creates a couple of providers inline, runs
  # the whole thing through logoscore, and scrapes /metrics.
  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./openmetrics;
      configFile = ./openmetrics/metadata.json;
      flakeInputs = inputs;
    };
}
