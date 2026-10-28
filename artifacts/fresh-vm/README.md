# Fresh-environment reproduction (2026-10-27)

1. `scripts/lima/create_vm.sh vtlab-fresh` created a new Ubuntu 24.04 VM and ran `scripts/setup_ubuntu.sh`, with nothing reused from the build VM.
2. Inside it, the repository was cloned to `~/fresh-clone` (branch `vehicle-lab`, commit `c7f7d82`).
3. `scripts/check_all.sh` ran in the clone.

The output folder is copied here from the clone: `20261027T220354Z/`.

All 13 steps passed:
- C++: 39 tests in each of the Debug, Release and ASan/UBSan builds;
- SIL: 42 tests in the Debug build and 42 in the ASan/UBSan build, none skipped;
- no sanitizer reports;
- the forced failure fails, and missing vcan permission fails clearly;
- all five deliberate breaks were caught;
- the demo ran.
