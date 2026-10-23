#!/usr/bin/env bash
# Metamorphic replay through the production seam (Codex N-03 and W3-B01; tools/replay/README.md, "Metamorphic
# captures"). Derives equivalent captures from one corpus, replays each through tests/dlssnr-loopback --replay-nr
# (one run.py invocation per capture; each takes the GPU lease itself, so do not wrap this in flock), moves each
# capture's runs out of artifacts/, and compares them variant by variant. Exit 0 only if every exact pair is identical
# in decisions and output bytes (outputs read in their own channel order).
#
#   tools/replay/metamorphic_seam.sh <OptiScaler.dll> [corpus capture.json] [out dir]
#
# METAMORPHIC_VARIANTS (default tools/replay/variants-metamorphic.json) and METAMORPHIC_SETS (default
# "geometry colour") select what runs; METAMORPHIC_DRY=1 only derives and validates the captures. Needs the loopback prerequisites of tests/dlssnr-loopback/README.md.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
dll=${1:?usage: metamorphic_seam.sh <OptiScaler.dll> [corpus capture.json] [out dir]}
corpus=${2:-$HOME/.cache/retoque-replay/baselines/b2eed6e3/corpus/capture.json}
out=${3:-$HOME/.cache/retoque-replay/metamorphic-$(date +%Y%m%d-%H%M%S)}
variants_file=${METAMORPHIC_VARIANTS:-$here/variants-metamorphic.json}
sets=${METAMORPHIC_SETS:-geometry colour}
mkdir -p "$out"
cd "$root"
transform() { python3 -m tools.replay.transform "$@" > /dev/null; }

captures=()
pairs=()
status=0
if [[ " $sets " == *" geometry "* ]]; then
    # The reference: the corpus as a v2 capture with nothing moved. The others must replay identically to it.
    [[ -d $out/identity ]] || transform pad "$corpus" --out "$out/identity" --depth-base 0 0 --motion-base 0 0 --pad 0 0
    [[ -d $out/padded ]] || transform pad "$corpus" --out "$out/padded" --depth-base 16 12 --motion-base 24 8 \
        --pad 8 20 --sentinel extreme
    [[ -d $out/padded-nan-units2 ]] || transform pad "$corpus" --out "$out/padded-nan-units2" --depth-base 3 1 \
        --motion-base 1 5 --pad 13 2 --sentinel nan --units 2
    [[ -d $out/units-neg-lowres ]] || transform pad "$corpus" --out "$out/units-neg-lowres" --depth-base 0 0 \
        --motion-base 0 0 --pad 0 0 --units -1 --mv-low-res on
    captures+=(identity padded padded-nan-units2 units-neg-lowres)
    pairs+=("identity padded" "identity padded-nan-units2" "identity units-neg-lowres")
fi
if [[ " $sets " == *" colour "* ]]; then
    # One logical sequence, its colour quantised once; BGRA8 bytes reordered. Resets on the first two scored swap
    # frames of the 7-frame schedule, in both orders, so a reset falls exactly where an alternating capture swaps.
    [[ -d $out/rgba8 ]] || transform colour8 "$corpus" --out "$out/rgba8" --order rgba --swap-every 7 \
        --swap-start 1 --reset-on-swap 2 5
    [[ -d $out/bgra8 ]] || transform colour8 "$corpus" --out "$out/bgra8" --order bgra --swap-every 7 \
        --swap-start 1 --reset-on-swap 2 5
    # Written for when the seam takes a per-frame colour format; run_replay.py refuses it today (README).
    [[ -d $out/alternate8 ]] || transform colour8 "$corpus" --out "$out/alternate8" --order alternate \
        --swap-every 7 --swap-start 1 --reset-on-swap 2 5 2> /dev/null
    captures+=(rgba8 bgra8)
    pairs+=("rgba8 bgra8")
fi

# METAMORPHIC_DRY=1: derive the captures and check each with the seam's own validator, without replaying anything.
if [[ ${METAMORPHIC_DRY:-0} == 1 ]]; then
    for c in "${captures[@]}" $([[ -d $out/alternate8 ]] && echo alternate8); do
        (cd tests/dlssnr-loopback && python3 -c 'import sys, run_replay; from pathlib import Path
try:
    run_replay.load_capture(Path(sys.argv[1])); print("accepted", sys.argv[2])
except SystemExit as e:
    print("refused", sys.argv[2] + ":", e)' "$out/$c/capture.json" "$c")
    done
    exit 0
fi

for c in "${captures[@]}"; do
    if [[ -d $out/$c/runs ]]; then
        echo "== $c: runs already present, not replayed again"
        continue
    fi
    echo "== replay $c"
    if ! LOOPBACK_REPLAY_CALIBRATE=0 LOOPBACK_REPLAY_JUDGE=0 \
        LOOPBACK_REPLAY_VARIANTS="$(cat "$variants_file")" \
        LOOPBACK_REPLAY_CAPTURE="$out/$c/capture.json" \
        python3 tests/dlssnr-loopback/run.py --replay-nr --dll "$dll" > "$out/$c/replay.log" 2>&1; then
        # A seam gate that fails is a result too: keep what ran and compare it.
        echo "SEAM FAIL $c: see $out/$c/replay.log (the lease held, a gate, or the run)"
        status=1
    fi
    report=tests/dlssnr-loopback/artifacts/replay-run/replay-report.json
    [[ -f $report ]] && cp "$report" "$out/$c/"
    [[ -d tests/dlssnr-loopback/artifacts/replay-run/runs ]] && mv tests/dlssnr-loopback/artifacts/replay-run/runs "$out/$c/runs"
done

for pair in "${pairs[@]}"; do
    read -r a b <<< "$pair"
    for v in $(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1]))))' "$variants_file"); do
        if [[ ! -d $out/$a/runs/$v || ! -d $out/$b/runs/$v ]]; then
            echo "MISSING $a vs $b, $v: a run is absent"
            status=1
            continue
        fi
        result=$(python3 -m tools.replay.transform compare "$out/$a/runs/$v" "$out/$b/runs/$v") && verdict=IDENTICAL ||
            { verdict=DIFFERS; status=1; }
        echo "$verdict $a vs $b, $v: $(python3 -c 'import json,sys; r=json.loads(sys.stdin.read()); print(
            "decisions differ at", r.get("decisions_differ"), "pixels differ on", r.get("pixels_differ_count"),
            "frames, first", r.get("pixels_differ", [])[:4])' <<< "$result")"
    done
done
# The A/A noise floor of the same capture: every-a against every-b, both every frame.
for c in "${captures[@]}"; do
    if [[ -d $out/$c/runs/every-a && -d $out/$c/runs/every-b ]]; then
        python3 -m tools.replay.transform compare "$out/$c/runs/every-a" "$out/$c/runs/every-b" > /dev/null &&
            echo "A/A $c: every-a = every-b" || echo "A/A $c: every-a and every-b DIFFER (the model is not repeatable here)"
    fi
done
echo "results in $out"
exit $status
