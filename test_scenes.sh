#!/usr/bin/env bash
#
# Render every scene in etc/scenes to test/out/<scene>.ppm and compare
# against the baseline images in test/scenes/<scene>.ppm.
#
# Generate the baselines first with: ./test_baseline.sh
#
# Usage: ./test_scenes.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RUN="./run.sh"
SCENES_DIR="etc/scenes"
BASELINE_DIR="test/scenes"
OUT_DIR="test/out"

if [[ ! -x "$RUN" ]]; then
  echo "error: $RUN not found or not executable" >&2
  exit 1
fi

if [[ ! -d "$BASELINE_DIR" ]]; then
  echo "error: baseline directory $BASELINE_DIR not found. Run ./test_baseline.sh first." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

shopt -s nullglob
scenes=("$SCENES_DIR"/*.flecs)
shopt -u nullglob

if [[ ${#scenes[@]} -eq 0 ]]; then
  echo "error: no scenes found in $SCENES_DIR" >&2
  exit 1
fi

render_failures=()
missing_baselines=()
mismatches=()
passed=0

for scene in "${scenes[@]}"; do
  name="$(basename "$scene" .flecs)"
  out="$OUT_DIR/$name.ppm"
  baseline="$BASELINE_DIR/$name.ppm"

  echo "==> $name"

  if ! "$RUN" --scene "$scene" --frame-out "$out"; then
    echo "    RENDER FAILED" >&2
    render_failures+=("$name")
    continue
  fi

  if [[ ! -f "$baseline" ]]; then
    echo "    no baseline at $baseline" >&2
    missing_baselines+=("$name")
    continue
  fi

  if cmp -s "$baseline" "$out"; then
    echo "    ok"
    passed=$((passed + 1))
  else
    echo "    MISMATCH (see $out vs $baseline)" >&2
    mismatches+=("$name")
  fi
done

echo
echo "Passed:           $passed / ${#scenes[@]}"
echo "Render failures:  ${#render_failures[@]}"
echo "Missing baseline: ${#missing_baselines[@]}"
echo "Mismatches:       ${#mismatches[@]}"

fail_total=$(( ${#render_failures[@]} + ${#missing_baselines[@]} + ${#mismatches[@]} ))
if [[ $fail_total -gt 0 ]]; then
  echo
  if [[ ${#render_failures[@]} -gt 0 ]]; then
    echo "Render failures:" >&2
    for f in "${render_failures[@]}"; do echo "  - $f" >&2; done
  fi
  if [[ ${#missing_baselines[@]} -gt 0 ]]; then
    echo "Missing baselines:" >&2
    for f in "${missing_baselines[@]}"; do echo "  - $f" >&2; done
  fi
  if [[ ${#mismatches[@]} -gt 0 ]]; then
    echo "Mismatches:" >&2
    for f in "${mismatches[@]}"; do echo "  - $f" >&2; done
  fi
  exit 1
fi

echo "All scenes match their baseline."
