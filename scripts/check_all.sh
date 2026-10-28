#!/usr/bin/env bash
# Full verification run, inside the Linux VM:
#   Debug, Release and ASan/UBSan builds with warnings as errors; every C++
#   and SIL test; the deliberate-break check; the failure-mode checks; the demo.
# Raw output goes to artifacts/<commit>/<run-id>/. Exits non-zero on any failure.
#
#   scripts/check_all.sh
set -uo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"
python="${LAB_PYTHON:-$HOME/.venvs/vtlab/bin/python}"
export LAB_PYTHON="$python"
commit="$(git rev-parse --short HEAD)"
run_id="$(date -u +%Y%m%dT%H%M%SZ)"
out="artifacts/$commit/$run_id"
asan_tmp="/tmp/vtlab-asan-$run_id"
mkdir -p "$out"
failures=0

step() {
    local name="$1"
    shift
    printf '%-26s' "$name"
    local started=$SECONDS
    if "$@" >"$out/$name.log" 2>&1; then
        echo "pass ($((SECONDS - started)) s)"
        echo "$name: pass" >>"$out/summary.txt"
    else
        echo "FAIL ($((SECONDS - started)) s), see $out/$name.log"
        echo "$name: FAIL" >>"$out/summary.txt"
        failures=$((failures + 1))
    fi
}

{
    echo "commit: $(git rev-parse HEAD)"
    echo "uncommitted changes outside artifacts/: $(git status --porcelain | grep -vc '^?? artifacts/')"
    echo "run id: $run_id"
    echo "kernel: $(uname -srm)"
    echo "os: $(. /etc/os-release && echo "$PRETTY_NAME")"
    echo "g++: $(g++ --version | head -1)"
    echo "cmake: $(cmake --version | head -1)"
    echo "mosquitto: $(mosquitto -h 2>&1 | head -1)"
    echo "packages:"
    dpkg-query -W -f '  ${Package} ${Version}\n' can-utils libmosquitto-dev nlohmann-json3-dev libgtest-dev \
        "linux-modules-extra-$(uname -r)"
    echo "python: $("$python" --version)"
    "$python" -m pip freeze | grep -iE '^(cantools|paho-mqtt|pytest|python-can)==' | sed 's/^/  /'
    echo "seeds: none. Tests use fixed inputs; run ids and gateway epochs are random per run."
} >"$out/environment.txt"

step build-debug bash -c "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build"
step ctest-debug ctest --test-dir build --output-on-failure --output-junit "$repo/$out/ctest-debug.xml"
step pytest-sil "$python" -m pytest -v -p no:cacheprovider --junitxml="$out/sil.xml"
step build-release bash -c "cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-release"
step ctest-release ctest --test-dir build-release --output-on-failure --output-junit "$repo/$out/ctest-release.xml"
step build-asan bash -c "cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLAB_SANITIZE=ON && cmake --build build-asan"
step ctest-asan ctest --test-dir build-asan --output-on-failure --output-junit "$repo/$out/ctest-asan.xml"
step pytest-sil-asan env LAB_BUILD_DIR="$repo/build-asan" "$python" -m pytest -v -p no:cacheprovider \
    --basetemp="$asan_tmp" --junitxml="$out/sil-asan.xml"
step sanitizer-reports bash -c "! grep -rlE 'AddressSanitizer|LeakSanitizer|runtime error:' '$asan_tmp'"
step forced-failure-fails bash -c "! LAB_FORCE_FAIL=1 ctest --test-dir build -R Harness"
step vcan-permission-failure scripts/check_vcan_failure.sh
step mutation-check scripts/mutation_check.sh
step demo scripts/run_demo.sh
rm -rf "$asan_tmp"

"$python" - "$out" >>"$out/summary.txt" <<'EOF'
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

out = Path(sys.argv[1])
print("\ntest counts (from JUnit XML):")
for name in ("ctest-debug.xml", "ctest-release.xml", "ctest-asan.xml", "sil.xml", "sil-asan.xml"):
    path = out / name
    if not path.exists():
        print(f"  {name}: missing")
        continue
    root = ET.parse(path).getroot()
    suites = [root] if root.tag == "testsuite" else root.findall("testsuite")
    total = sum(int(s.get("tests", 0)) for s in suites)
    failed = sum(int(s.get("failures", 0)) + int(s.get("errors", 0)) for s in suites)
    skipped = sum(int(s.get("skipped", 0)) for s in suites)
    print(f"  {name}: {total} run, {failed} failed, {skipped} skipped")
EOF

echo
cat "$out/summary.txt"
echo
echo "Raw output: $out"
exit $((failures > 0))
