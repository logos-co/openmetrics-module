{
  description = "prometheus_metrics — a Logos module that serves a Prometheus /metrics endpoint by querying configured modules' collectMetrics()";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/c_ffi";
    logos-module-builder.inputs.logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk/c_ffi";
    logos-rust-sdk.url = "github:logos-co/logos-rust-sdk";
    logos-logoscore-cli.url = "github:logos-co/logos-logoscore-cli";
    nixpkgs.follows = "logos-module-builder/nixpkgs";
  };

  outputs = inputs@{ self, logos-module-builder, logos-rust-sdk, logos-logoscore-cli, nixpkgs, ... }:
    let
      mkModule = logos-module-builder.lib.mkLogosModule;
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = fn: nixpkgs.lib.genAttrs systems fn;
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};

          # Provides extraBuildInputs (liblogos_module_client) and a setupHook
          # that exports LOGOS_MODULE_CLIENT_ROOT for CMake.
          rustSdkBuild = logos-rust-sdk.lib.callerBuildSupport.${system};

          # Assemble a source tree matching the Cargo.toml path layout:
          #   rust-lib/           (Cargo.toml, Cargo.lock, src/, include/)
          #   logos-rust-sdk-src/ (the SDK crate — resolves the path dependency)
          rustSrc = pkgs.runCommand "prometheus-metrics-rust-src" { } ''
            mkdir -p $out
            cp -r ${./rust-lib} $out/rust-lib
            cp -r ${logos-rust-sdk} $out/logos-rust-sdk-src
          '';

          # Build the Rust staticlib (libprometheus_metrics.a). logos_sdk_*
          # symbols stay unresolved here; CMake links liblogos_module_client to
          # satisfy them at the final plugin link.
          rustLib = pkgs.rustPlatform.buildRustPackage {
            pname = "prometheus_metrics";
            version = "0.1.0";
            src = rustSrc;
            sourceRoot = "prometheus-metrics-rust-src/rust-lib";
            cargoLock.lockFile = ./rust-lib/Cargo.lock;
            doCheck = false;
          };

          metricsModule = mkModule {
            src = ./.;
            configFile = ./metadata.json;
            flakeInputs = inputs;

            extraBuildInputs = rustSdkBuild.extraBuildInputs;

            preConfigure = ''
              echo "=== Staging prebuilt Rust static library ==="
              mkdir -p lib
              cp ${rustLib}/lib/libprometheus_metrics.a lib/
              cp rust-lib/include/prometheus_metrics.h lib/
              echo "=== Rust static library staged ==="

              ${rustSdkBuild.setupHook}
            '';
          };

          # Example provider module (std-only Rust) that implements
          # collectMetrics(); used to demonstrate and validate the scraper.
          demoModule = mkModule {
            src = ./example-metrics-source;
            configFile = ./example-metrics-source/metadata.json;
            flakeInputs = inputs;

            preConfigure = ''
              echo "=== Building metrics_demo Rust library ==="
              export HOME=$TMPDIR
              export CARGO_HOME=$TMPDIR/cargo
              mkdir -p $CARGO_HOME

              pushd rust-lib
              cargo build --release --offline
              popd

              mkdir -p lib
              cp rust-lib/target/release/libmetrics_demo.a lib/
              cp rust-lib/include/metrics_demo.h lib/
              echo "=== metrics_demo built ==="
            '';
          };

          # A single modules/ directory holding both modules, for `logoscore -m`.
          allModules = pkgs.runCommand "prometheus-metrics-all-modules" { } ''
            mkdir -p $out
            for src in ${metricsModule.packages.${system}.install} ${demoModule.packages.${system}.install}; do
              cp -rL "$src"/modules/* $out/ 2>/dev/null || true
            done
          '';
        in
        {
          prometheus_metrics         = metricsModule.packages.${system}.default;
          prometheus_metrics_install = metricsModule.packages.${system}.install;
          metrics_demo               = demoModule.packages.${system}.default;
          metrics_demo_install       = demoModule.packages.${system}.install;
          # Convenience: a modules/ directory (both modules) for `logoscore -m`.
          modules                    = allModules;
          default                    = metricsModule.packages.${system}.default;
        }
      );

      checks = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          # Rust unit tests (formatter exposition-format coverage). Uses the
          # assembled source tree so the logos-rust-sdk path dep resolves;
          # logos_sdk_* symbols are stubbed under cfg(test) in lib.rs.
          rust-unit = pkgs.rustPlatform.buildRustPackage {
            pname = "prometheus_metrics-tests";
            version = "0.1.0";
            src = pkgs.runCommand "prometheus-metrics-rust-src" { } ''
              mkdir -p $out
              cp -r ${./rust-lib} $out/rust-lib
              cp -r ${logos-rust-sdk} $out/logos-rust-sdk-src
            '';
            sourceRoot = "prometheus-metrics-rust-src/rust-lib";
            cargoLock.lockFile = ./rust-lib/Cargo.lock;
            buildPhase = "cargoCheckType=release cargo test --release --offline";
            installPhase = "touch $out";
          };
        }
      );
    };
}
