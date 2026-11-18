#!/bin/bash
# LCG2 -- SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michele Giugliano
#
# Build every program with AddressSanitizer and UndefinedBehaviorSanitizer and run all the
# test suites against those builds:   make sanitize     (or: bash tests/sanitize.sh)
#
# This is a script of the repository, and not something to retype, because the details
# matter and a private copy that drifts gives false failures: the reference `sg` must be
# built with -ffp-contract=off (else the noise differs in the last bits), and the pinned
# StimGen commit and fingerprint must be the real ones (the tests compare them).
#
# Variables:  H5CC (the HDF5 compiler wrapper), STIMGEN_SRC (default ../StimGen/src),
#             SAN_DIR (where the builds go; default a fresh folder under $TMPDIR).
# HDF5 itself is not instrumented. Leak detection is off (macOS has none); see
# docs/10-architecture.md for how leaks are checked there.
set -e
cd "$(dirname "$0")/.."
SG=${STIMGEN_SRC:-../StimGen/src}
A=${SAN_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/lcg2_san.XXXXXX")}
H5=$H5CC
if [ -z "$H5" ]; then
  for c in h5cc /opt/homebrew/opt/hdf5-mpi/bin/h5cc /opt/homebrew/opt/hdf5/bin/h5cc /usr/local/opt/hdf5-mpi/bin/h5cc /usr/local/opt/hdf5/bin/h5cc; do
    H5=$(command -v "$c") && break          # command -v never colours its output, unlike ls
  done
fi
[ -n "$H5" ] || { echo "sanitize: no HDF5 compiler wrapper found (set H5CC=...)"; exit 1; }
INI="-DINI_HANDLER_LINENO=1 -DINI_CALL_HANDLER_ON_NEW_SECTION=1 -DINI_ALLOW_MULTILINE=0 -DINI_STOP_ON_FIRST_ERROR=1 -DINI_MAX_LINE=1000"
F="-std=c99 -Wall -Wextra -g -O1 -ffp-contract=off -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wno-deprecated-declarations"
SGSRC="$SG/util.c $SG/help.c $SG/render.c $SG/rng.c $SG/parse.c $SG/prims.c $SG/eval.c $SG/protocol.c $SG/sgb.c $SG/sg_api.c"
CORE="src/err.c src/units.c src/config.c src/cli.c src/json.c src/stim.c src/platform.c src/backend_sim.c src/backend_neuron.c src/engine.c src/sha256.c src/archive.c src/session.c vendor/inih/ini.c"
APP="src/record.c src/run.c"
PIN_COMMIT=$(sed -n 's/^commit *= *//p' STIMGEN_PIN); PIN_SHA=$(sed -n 's/^sources_sha256 *= *//p' STIMGEN_PIN)
PIN="-DLCG2_STIMGEN_COMMIT=\"$PIN_COMMIT\" -DLCG2_STIMGEN_SHA256=\"$PIN_SHA\" -DLCG2_NEURON_ADAPTER=\"$(pwd)/neuron/lcg2_neuron.py\""
build() { n=$1; shift; "$@" > "$A/$n.log" 2>&1 || { echo "sanitize: building $n failed:"; head -10 "$A/$n.log"; exit 1; }; }
build lcg2       $H5 $F $INI $PIN -Isrc -Ivendor/inih -o $A/lcg2 $CORE $APP src/main.c $SGSRC -lm
build runtest    $H5 $F $INI $PIN -Isrc -Ivendor/inih -o $A/runtest $CORE $APP tests/runtest.c $SGSRC -lm
build recordtest $H5 $F $INI -Isrc -Ivendor/inih -o $A/recordtest $CORE src/record.c tests/recordtest.c $SGSRC -lm
build unit       cc $F $INI $PIN -Isrc -Ivendor/inih -o $A/unit $CORE tests/unit.c $SGSRC -lm
build stimdump   cc $F $INI $PIN -Isrc -Ivendor/inih -o $A/stimdump $CORE tests/stimdump.c $SGSRC -lm
build enginetest cc $F $INI $PIN -Isrc -Ivendor/inih -o $A/enginetest $CORE tests/enginetest.c $SGSRC -lm
build jsontool   cc $F -Isrc -o $A/jsontool tests/jsontool.c src/json.c src/err.c -lm
build sg         cc -std=c99 -O1 -g -ffp-contract=off -fsanitize=address -Wno-deprecated-declarations -I$SG -o $A/sg $SG/main.c $SGSRC -lm
export ASAN_OPTIONS=detect_leaks=0
echo "sanitizer builds in $A"
$A/unit | tail -1
python3 tests/run_tests.py $A/lcg2 2>&1 | tail -1
LCG2_BUILD=$A LCG2_BIN=$A/lcg2 python3 tests/test_stim.py 2>&1 | tail -1
LCG2_BUILD=$A LCG2_BIN=$A/lcg2 python3 tests/test_engine.py 2>&1 | tail -1
LCG2_BUILD=$A LCG2_BIN=$A/lcg2 python3 tests/test_record.py 2>&1 | tail -1
LCG2_BUILD=$A LCG2_BIN=$A/lcg2 python3 tests/test_neuron.py 2>&1 | tail -1
LCG2_BUILD=$A LCG2_BIN=$A/lcg2 python3 tests/test_multipatch.py 2>&1 | tail -1
python3 tests/check_clean.py | tail -1
