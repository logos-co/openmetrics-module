#!/usr/bin/env bash
#
# Execute the openmetrics doc-test end-to-end and regenerate its Markdown.
#
# Spec:
#   - openmetrics.test.yaml   builds the openmetrics scraper from this repo plus
#                             two inline provider modules, runs all three through
#                             a logoscore daemon, and scrapes /metrics.
#
# The runner is the shared `doctest` CLI (https://github.com/logos-co/logos-doctest),
# invoked directly via its flake. `doctest run` executes every command in a temp
# directory and asserts on the output; `doctest generate` renders the spec to
# Markdown under outputs/; `doctest clean` strips build artifacts.
#
# To run against a local logos-doctest checkout instead of the published flake,
# set DOCTEST, e.g.:  DOCTEST="nix run path:../../logos-doctest --" ./run.sh
#
set -euo pipefail

cd "$(dirname "$0")"

# The doctest CLI. Override by exporting DOCTEST (space-separated command).
read -r -a DOCTEST <<< "${DOCTEST:-nix run github:logos-co/logos-doctest --}"
OUTPUT_DIR="./outputs"
SPECS=(
  "openmetrics.test.yaml"
)

# Build the doc-test against THIS repo's current commit rather than the latest
# published flake. The spec builds `github:logos-co/openmetrics-module{release}#lgx`,
# and the pin below makes {release} expand to $COMMIT — so the openmetrics module
# under test is exactly what's checked out here. Override by exporting COMMIT, or
# set COMMIT="" to fall back to latest main.
#
# Note: nix fetches the commit from the GitHub remote, so $COMMIT must be pushed
# to logos-co/openmetrics-module. A local-only / uncommitted HEAD won't resolve;
# export COMMIT="" (or push first) in that case.
COMMIT="${COMMIT-$(git rev-parse HEAD)}"
RELEASE_FOR=()
if [ -n "${COMMIT}" ]; then
  RELEASE_FOR=(--release-for "openmetrics-module=${COMMIT}")
  echo "==> Pinning openmetrics-module to ${COMMIT}"
else
  echo "==> COMMIT empty; building against latest openmetrics-module main"
fi

echo "==> Clearing previous ${OUTPUT_DIR}/"
if [ -e "${OUTPUT_DIR}" ]; then
  chmod -R u+w "${OUTPUT_DIR}" 2>/dev/null || true
fi
rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"

for SPEC in "${SPECS[@]}"; do
  name="$(basename "${SPEC%.test.yaml}")"
  spec_out="${OUTPUT_DIR}/${name}"
  mkdir -p "${spec_out}"

  echo "==> Running ${SPEC} into ${spec_out}/"
  "${DOCTEST[@]}" run "${SPEC}" \
    --verbose \
    --continue-on-fail \
    ${RELEASE_FOR[@]+"${RELEASE_FOR[@]}"} \
    --output-dir "${spec_out}/"

  echo "==> Generating ${OUTPUT_DIR}/${name}.md"
  "${DOCTEST[@]}" generate "${SPEC}" \
    ${RELEASE_FOR[@]+"${RELEASE_FOR[@]}"} \
    -o "${OUTPUT_DIR}/${name}.md"
done

echo "==> Cleaning build artifacts from ${OUTPUT_DIR}/"
"${DOCTEST[@]}" clean "${OUTPUT_DIR}" --verbose

echo "==> Done. Rendered docs are in ${OUTPUT_DIR}/"
