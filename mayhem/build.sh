#!/usr/bin/env bash
#
# mayhem/build.sh — build nanomsg three ways, all out of tree under build/ (gitignored upstream):
#   build/fuzz   sanitized + SanCov static libnanomsg, linked into the three transport harnesses
#   build/test   upstream's own test suite with NORMAL flags, plus the mayhem/kat.c known-answer probe
# Runs inside the commit image as `mayhem` in /mayhem. nanomsg has no third-party dependencies, so
# nothing is fetched; re-running is safe and works offline.
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' (empty) — it must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${STANDALONE_FUZZ_MAIN:=/opt/mayhem/StandaloneFuzzTargetMain.c}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

# 1) Sanitized library. fuzzer-no-link puts SanCov in the library itself (not only the harness TU),
#    unconditionally, so the target still records coverage when SANITIZER_FLAGS is empty.
FUZZ_CFLAGS="-O1 $SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS"
cmake -S . -B build/fuzz -G Ninja \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_C_FLAGS="$FUZZ_CFLAGS" \
    -DNN_STATIC_LIB=ON -DNN_TESTS=OFF -DNN_TOOLS=OFF -DNN_ENABLE_NANOCAT=OFF -DNN_ENABLE_DOC=OFF
cmake --build build/fuzz -j"$MAYHEM_JOBS"

# 2) Harnesses: libFuzzer binary + a run-once standalone reproducer per transport, LSan off at build time.
SYSLIBS="-lanl -lpthread"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.c" -o build/fuzz/lsan_off.o
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o build/fuzz/standalone_main.o
for t in ipc tcp ws; do
    def="-DFUZZ_$(echo "$t" | tr a-z A-Z)"
    $CC -O1 $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE -DNN_STATIC_LIB $def \
        "$SRC/mayhem/fuzz_transport.c" build/fuzz/lsan_off.o build/fuzz/libnanomsg.a $SYSLIBS \
        -o "/mayhem/fuzz_$t"
    $CC -O1 $SANITIZER_FLAGS $DEBUG_FLAGS -DNN_STATIC_LIB $def \
        "$SRC/mayhem/fuzz_transport.c" build/fuzz/standalone_main.o build/fuzz/lsan_off.o \
        build/fuzz/libnanomsg.a $SYSLIBS -o "/mayhem/fuzz_$t-standalone"
done

# 3) Upstream's test suite and the KAT probe, NORMAL flags (no sanitizer, no -gdwarf-3).
cmake -S . -B build/test -G Ninja \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_C_FLAGS="$COVERAGE_FLAGS" \
    -DNN_STATIC_LIB=ON -DNN_TESTS=ON -DNN_TOOLS=OFF -DNN_ENABLE_NANOCAT=OFF -DNN_ENABLE_DOC=OFF
cmake --build build/test -j"$MAYHEM_JOBS"
$CC -O1 $COVERAGE_FLAGS -DNN_STATIC_LIB "$SRC/mayhem/kat.c" build/test/libnanomsg.a $SYSLIBS \
    -o build/test/kat
