#!/usr/bin/env bash
# Checks that the SIL tests fail with a clear message, instead of skipping,
# when a vcan interface can't be created. A stand-in `sudo` refuses, as it
# would without the needed permission.
set -uo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
python="${LAB_PYTHON:-$HOME/.venvs/vtlab/bin/python}"
shim="$(mktemp -d)"
trap 'rm -rf "$shim"' EXIT

cat >"$shim/sudo" <<'EOF'
#!/bin/sh
echo "sudo: a password is required" >&2
exit 1
EOF
chmod +x "$shim/sudo"

output="$(cd "$repo" && PATH="$shim:$PATH" "$python" -m pytest -p no:cacheprovider -q -rA tests/sil/test_t1_vcan.py 2>&1)"
status=$?
echo "$output"
echo
if [ "$status" -eq 0 ]; then
    echo "FAIL: tests passed although vcan could not be created"
    exit 1
fi
if ! grep -q "could not create vcan interface" <<<"$output"; then
    echo "FAIL: no clear message about vcan"
    exit 1
fi
if grep -qiE "[0-9]+ skipped" <<<"$output"; then
    echo "FAIL: tests were skipped instead of failing"
    exit 1
fi
echo "OK: the test failed with a clear vcan message and nothing was skipped"
