#!/usr/bin/env bash
#
# mayhem/test.sh — RUN nanomsg's own ctest suite and the mayhem/kat.c known-answer probe (both built
# by mayhem/build.sh). ctest only judges exit codes, so the probe's output is compared from bash against
# fixed expected values: a neutered or broken library cannot pass.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}


BUILD="$SRC/build/test"
[ -x "$BUILD/kat" ] || { echo "missing $BUILD/kat — build.sh did not build the probe" >&2; emit_ctrf "nanomsg-kat" 0 1; exit 1; }
[ -f "$BUILD/CTestTestfile.cmake" ] || { echo "missing ctest tree in $BUILD" >&2; emit_ctrf "nanomsg-ctest" 0 1; exit 1; }

passed=0 ; failed=0

# 1) Known-answer probe: every expected line must appear in the probe's stdout.
kat_out="$("$BUILD/kat" 2>&1)"
echo "$kat_out"
while IFS= read -r want; do
  if printf '%s\n' "$kat_out" | grep -qxF -- "$want"; then passed=$((passed+1))
  else echo "KAT MISSING: $want" >&2; failed=$((failed+1)); fi
done <<'EXPECTED'
pair-iovec: 13 hello nanomsg
reqrep: echo:ABC
pipeline: 1 2 3
ws-status: HTTP/1.1 101 Switching Protocols
ws-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
ws-frame: 2 hi
KAT-DONE
EXPECTED

# 2) Upstream's suite (one ctest case per test program).
ctest_out="$(cd "$BUILD" && ctest --output-on-failure 2>&1)"
echo "$ctest_out" | tail -15
summary="$(printf '%s\n' "$ctest_out" | sed -n 's/.*tests passed, \([0-9]*\) tests failed out of \([0-9]*\).*/\1 \2/p' | tail -1)"
if [ -z "$summary" ]; then
  echo "ctest produced no summary" >&2; failed=$((failed+1))
else
  set -- $summary
  nfail=$1 ; ntotal=$2
  failed=$((failed+nfail)) ; passed=$((passed+ntotal-nfail))
  [ "$ntotal" -ge 40 ] || { echo "ctest ran only $ntotal tests (expected 44)" >&2; failed=$((failed+1)); }
fi

emit_ctrf "nanomsg-ctest+kat" "$passed" "$failed" 0
