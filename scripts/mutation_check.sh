#!/usr/bin/env bash
# Breaks the code on purpose, one change at a time, in a scratch copy of the
# repository, and checks that the named tests fail for each break. Exits
# non-zero if any break goes unnoticed. Run inside the Linux VM.
#
#   scripts/mutation_check.sh [output-file]
set -uo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
python="${LAB_PYTHON:-$HOME/.venvs/vtlab/bin/python}"
out="${1:-}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
missed=0

log() {
    echo "$*"
    if [ -n "$out" ]; then echo "$*" >>"$out"; fi
}

# mutate NAME FILE OLD NEW TEST...
mutate() {
    local name="$1" file="$2" old="$3" new="$4"
    shift 4
    local dir="$work/$name"
    mkdir -p "$dir"
    tar -C "$repo" --exclude='./build*' --exclude=./.git -cf - . | tar -C "$dir" -xf -
    if ! python3 - "$dir/$file" "$old" "$new" <<'EOF'
import sys
path, old, new = sys.argv[1:]
text = open(path).read()
if text.count(old) != 1:
    sys.exit(f"mutation target found {text.count(old)} times in {path}")
open(path, "w").write(text.replace(old, new))
EOF
    then
        log "$name: could not apply"
        missed=1
        return
    fi
    if ! (cmake -S "$dir" -B "$dir/build" -G Ninja -DLAB_WERROR=OFF >/dev/null && cmake --build "$dir/build" >/dev/null 2>&1); then
        log "$name: build failed"
        missed=1
        return
    fi
    local caught=()
    for test in "$@"; do
        if ! (cd "$dir" && LAB_BUILD_DIR="$dir/build" bash -c "$test") >"$dir/test.log" 2>&1; then
            caught+=("$test")
        fi
    done
    if [ ${#caught[@]} -eq 0 ]; then
        log "$name: NOT CAUGHT by: $*"
        missed=1
    else
        log "$name: caught (${#caught[@]}/$# tests failed):"
        for test in "${caught[@]}"; do log "    $test"; done
    fi
}

pytest="$python -m pytest -q -p no:cacheprovider"

log "# Mutation check $(date -u +%Y-%m-%dT%H:%M:%SZ), commit $(git -C "$repo" rev-parse --short HEAD)"

mutate expiry_never_fires controller/lighting_controller.cpp \
    "if (mode_ == Mode::Normal && nowMs - lastCommandMs_ > timeoutMs_)" \
    "if (false && mode_ == Mode::Normal && nowMs - lastCommandMs_ > timeoutMs_)" \
    "ctest --test-dir build -R ControllerTable" \
    "$pytest tests/sil/test_t3_controller.py -k 'commands_that_stop or timeout_fires'" \
    "$pytest tests/sil/test_t5_faults.py -k commands_stopping"

mutate receipt_sent_before_save collector/mqtt_collector.cpp \
    "    collector.crash.reach(\"collector_before_persist\", collector.trace);" \
    "    SendReceipt(client, collector, parsed, lab::ReceiptPayload(parsed, {IngestStatus::Stored, \"\"}));
    collector.crash.reach(\"collector_before_persist\", collector.trace);" \
    "$pytest tests/sil/test_t5_faults.py -k crash_before_saving" \
    "$pytest tests/sil/test_t5_faults.py -k full_disk"

mutate repeated_alive_resets_timer controller/lighting_controller.cpp \
    "if (aliveValid_ && command.alive == lastAlive_)" \
    "if (false)" \
    "ctest --test-dir build -R ControllerTable" \
    "$pytest tests/sil/test_t3_controller.py -k repeated"

mutate duplicates_not_detected src/MetricsStore.cpp \
    "if (existing != byIdentity_.end())" \
    "if (false)" \
    "ctest --test-dir build -R CheckedStore.Duplicates" \
    "$pytest tests/sil/test_t4_pipeline.py -k retry"

mutate incomplete_record_not_cut_off src/WriteAheadLog.cpp \
    "if (ops_.truncate(fd_, static_cast<off_t>(offset)) != 0 || ops_.sync(fd_) != 0)" \
    "if (false)" \
    "ctest --test-dir build -R CheckedStore"

if [ "$missed" -eq 0 ]; then log "All mutations were caught."; else log "Some mutations were NOT caught."; fi
exit "$missed"
