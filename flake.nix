{
  description = "openmetrics — a Logos module that serves an OpenMetrics /metrics endpoint by scraping modules that implement collectMetrics()";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    logos-logoscore-cli.url = "github:logos-co/logos-logoscore-cli";
    nixpkgs.follows = "logos-nix/nixpkgs";
  };

  outputs = inputs@{ self, logos-nix, logos-module-builder, logos-logoscore-cli, nixpkgs }:
    let
      mkModule = logos-module-builder.lib.mkLogosModule;
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = fn: nixpkgs.lib.genAttrs systems fn;

      # The scraper. Declares an interface_dependency on `metrics_source`
      # (interfaces/metrics_source.h) — no concrete module dependency.
      openmetrics = mkModule {
        src = ./openmetrics;
        configFile = ./openmetrics/metadata.json;
        flakeInputs = inputs;
      };

      # Example providers implementing collectMetrics(), for demos/tests.
      demo = mkModule {
        src = ./example-metrics-source;
        configFile = ./example-metrics-source/metadata.json;
        flakeInputs = inputs;
      };
      demo2 = mkModule {
        src = ./example-metrics-source-2;
        configFile = ./example-metrics-source-2/metadata.json;
        flakeInputs = inputs;
      };
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };

          # A single modules/ directory with all modules, for `logoscore -m`.
          modulesDir = pkgs.runCommand "openmetrics-modules" { } ''
            mkdir -p $out
            for inst in ${openmetrics.packages.${system}.install} \
                        ${demo.packages.${system}.install} \
                        ${demo2.packages.${system}.install}; do
              if [ -d "$inst/modules" ]; then
                cp -rn "$inst/modules/." "$out/"
              fi
            done
          '';
        in
        {
          openmetrics            = openmetrics.packages.${system}.default;
          openmetrics_install    = openmetrics.packages.${system}.install;
          metrics_demo           = demo.packages.${system}.default;
          metrics_demo_install   = demo.packages.${system}.install;
          metrics_demo_2         = demo2.packages.${system}.default;
          metrics_demo_2_install = demo2.packages.${system}.install;
          modules                = modulesDir;
          default                = openmetrics.packages.${system}.default;
        }
      );

      # Integration "doctest": load the openmetrics scraper plus two example
      # provider modules under a logoscore daemon, start the server, and scrape
      # /metrics — asserting the aggregated OpenMetrics document.
      #
      # NOTE: requires the logos-cpp-sdk thread-safe-IPC change (the scraper's
      # HTTP thread calls collectMetrics on other modules, which the SDK
      # marshals to the module's main thread). Run against a dirty/patched
      # logos-cpp-sdk until that lands on master, e.g.:
      #   nix build .#checks.<system>.integration \
      #     --override-input logos-module-builder/logos-cpp-sdk path:/path/to/logos-cpp-sdk
      checks = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          logoscore = logos-logoscore-cli.packages.${system}.default;
          modulesDir = self.packages.${system}.modules;
        in
        {
          integration = pkgs.runCommand "openmetrics-integration-test"
            {
              nativeBuildInputs = [ logoscore pkgs.curl ]
                ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.qt6.qtbase ];
            }
            ''
              set -eu
              export QT_QPA_PLATFORM=offscreen
              export QT_FORCE_STDERR_LOGGING=1
              export HOME=$TMPDIR
              ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
                export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
              ''}
              CFG=$TMPDIR/cfg
              PORT=19099
              mkdir -p "$CFG" "$out"

              echo "=== start logoscore daemon ==="
              logoscore -D -m ${modulesDir} --config-dir "$CFG" &
              DAEMON=$!
              for _ in $(seq 1 60); do
                logoscore --config-dir "$CFG" status >/dev/null 2>&1 && break || sleep 1
              done

              echo "=== load the three modules ==="
              logoscore --config-dir "$CFG" load-module metrics_demo
              logoscore --config-dir "$CFG" load-module metrics_demo_2
              logoscore --config-dir "$CFG" load-module openmetrics

              echo "=== start the openmetrics server ==="
              logoscore --config-dir "$CFG" call openmetrics start \
                "{\"port\":$PORT,\"modules\":[\"metrics_demo\",\"metrics_demo_2\"]}"

              echo "=== scrape /metrics ==="
              BODY=$(curl -fsS -m 20 "http://127.0.0.1:$PORT/metrics")
              echo "$BODY" | tee "$out/metrics.txt"

              echo "=== assert OpenMetrics output ==="
              grep -q '# TYPE demo_scrapes counter'                                   <<<"$BODY"
              grep -q 'demo_scrapes_total{module="metrics_demo"}'                      <<<"$BODY"
              grep -q 'demo_temperature_celsius{module="metrics_demo",sensor="core"}'  <<<"$BODY"
              grep -q 'demo2_messages_total{module="metrics_demo_2",topic="chat"}'     <<<"$BODY"
              grep -q 'demo2_peers{module="metrics_demo_2"}'                           <<<"$BODY"
              grep -q '^# EOF$'                                                        <<<"$BODY"

              echo "=== assert /health ==="
              test "$(curl -fsS -m 5 "http://127.0.0.1:$PORT/health")" = "ok"

              echo "=== shut down ==="
              logoscore --config-dir "$CFG" call openmetrics stop || true
              logoscore --config-dir "$CFG" stop || true
              kill $DAEMON 2>/dev/null || true

              echo PASS > "$out/result"
            '';
        }
      );
    };
}
