# LCG2 -- SPDX-License-Identifier: MIT
#
# Copyright (c) 2026 Michele Giugliano
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# LCG2 -- Makefile.
#   make              check the StimGen pin, build ./lcg2 and the test tools
#   make test         run the tests (needs python3)
#   make check-pin    only compare the StimGen sources with STIMGEN_PIN
#   make sanitize     build with AddressSanitizer and UBSan and run every suite against that
#   make clean        remove the build folder and ./lcg2
#
# Variables you may set on the command line:
#   CC            the C compiler (default cc)
#   BUILD         the folder for object files and test tools (default build). Put it
#                 outside a Dropbox folder if the link fails with "Resource deadlock
#                 avoided": Dropbox holds freshly written files. Example:
#                     make BUILD=/tmp/lcg2-build
#                 (tests/test_stim.py then needs LCG2_BUILD=/tmp/lcg2-build)
#   STIMGEN_SRC   where the StimGen 2 library sources are (default ../StimGen/src)
#
# StimGen is NOT copied into this project.  Its library files are compiled from
# STIMGEN_SRC into $(BUILD)/, with LCG2's own flags, and only after the sources
# have been compared with the fingerprint in STIMGEN_PIN.  Nothing is ever
# written inside the StimGen folder (it has stale .o files of its own; they
# are not used).  The library is not linked into lcg2 yet (that is milestone
# M2), but it is compiled now so that the flags and the warnings are proved.

CC     ?= cc
BUILD  ?= build
# The flags of StimGen's own Makefile, so that the rendered samples are the
# same (-ffp-contract=off: no fused multiply-add, results do not depend on the
# compiler's choice).
CFLAGS ?= -std=c99 -pedantic -Wall -Wextra -O2
CFLAGS += -ffp-contract=off
LDLIBS  = -lm

# inih (vendor/inih) is configured by macros, and these change the handler's
# prototype, so ini.c and config.c must be compiled with the same set.
# See the comment at the top of src/config.c for what each one does.
INIFLAGS = -DINI_HANDLER_LINENO=1 -DINI_CALL_HANDLER_ON_NEW_SECTION=1 \
           -DINI_ALLOW_MULTILINE=0 -DINI_STOP_ON_FIRST_ERROR=1 \
           -DINI_MAX_LINE=1000

STIMGEN_SRC ?= ../StimGen/src
# The library files of StimGen, in the order used to compute the fingerprint.
SG_FILES = util help render rng parse prims eval protocol sgb sg_api

# Every object depends on every header: a change to a struct in one header must rebuild
# everything that was compiled with the old layout (a stale object with a different struct
# size is a silent memory bug, and an `Err` that grew once caused exactly that).
HDRS = $(wildcard src/*.h)

# The modules that need nothing but the C library, and those that need HDF5 (record.c
# only: HDF5 is kept out of every test tool that does not write a data file).
CORE    = err units config cli json stim platform backend_sim engine sha256 archive session
APP     = record run main
OBJ     = $(CORE:%=$(BUILD)/%.o) $(BUILD)/ini.o $(APP:%=$(BUILD)/%.o)
CORE_OBJ= $(CORE:%=$(BUILD)/%.o) $(BUILD)/ini.o
SG_OBJ  = $(SG_FILES:%=$(BUILD)/stimgen/%.o)

# HDF5 (serial API, calls since 1.10).  A wrapper compiler knows its include and
# library paths: h5cc (on the PATH, e.g. libhdf5-dev on Linux), or the one inside a
# Homebrew keg.  The Mac has the parallel build (hdf5-mpi), whose wrapper uses mpicc: the
# serial calls work in it.  Override with H5CC=/path/to/h5cc.
H5CC ?= $(shell command -v h5cc 2>/dev/null)
ifeq ($(H5CC),)
H5CC := $(firstword $(wildcard /opt/homebrew/opt/hdf5-mpi/bin/h5cc /opt/homebrew/opt/hdf5/bin/h5cc \
                               /usr/local/opt/hdf5-mpi/bin/h5cc /usr/local/opt/hdf5/bin/h5cc))
endif

# The StimGen version this build is pinned to, recorded in every data file.
PIN_COMMIT := $(shell sed -n 's/^commit *= *//p' STIMGEN_PIN)
PIN_SHA    := $(shell sed -n 's/^sources_sha256 *= *//p' STIMGEN_PIN)
PINFLAGS    = -DLCG2_STIMGEN_COMMIT='"$(PIN_COMMIT)"' -DLCG2_STIMGEN_SHA256='"$(PIN_SHA)"'

all: check-pin check-hdf5 lcg2 tools

lcg2: $(OBJ) $(SG_OBJ)
	$(H5CC) $(CFLAGS) -o $@ $(OBJ) $(SG_OBJ) $(LDLIBS)

$(BUILD)/%.o: src/%.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(INIFLAGS) -Isrc -Ivendor/inih -c -o $@ $<

# record.c is the one file compiled with HDF5's wrapper, which adds its include path.
$(BUILD)/record.o: src/record.c $(HDRS) | $(BUILD)
	$(H5CC) $(CFLAGS) -Isrc -c -o $@ $<

# run.c knows which StimGen it was built against.
$(BUILD)/run.o: src/run.c $(HDRS) STIMGEN_PIN | $(BUILD)
	$(CC) $(CFLAGS) $(PINFLAGS) -Isrc -c -o $@ $<

$(BUILD)/ini.o: vendor/inih/ini.c vendor/inih/ini.h | $(BUILD)
	$(CC) $(CFLAGS) $(INIFLAGS) -c -o $@ $<

# The StimGen library, compiled into $(BUILD)/stimgen/ (used from M2 on).
stimgen-lib: check-pin $(SG_OBJ)

$(BUILD)/stimgen/%.o: $(STIMGEN_SRC)/%.c $(STIMGEN_SRC)/sg.h | $(BUILD)/stimgen
	$(CC) $(CFLAGS) -I$(STIMGEN_SRC) -c -o $@ $<

$(BUILD) $(BUILD)/stimgen:
	mkdir -p $@

# ---------------------------------------------------------------------------
# Tools used by the tests (not part of lcg2):
#   $(BUILD)/sg         StimGen's own command-line renderer, compiled from
#                    STIMGEN_SRC with LCG2's flags, to compare with
#   $(BUILD)/stimdump   dumps what LCG2's stimulus module holds, for comparison
#   $(BUILD)/jsontool   round-trips JSON through json.c
# ---------------------------------------------------------------------------
tools: $(BUILD)/sg $(BUILD)/stimdump $(BUILD)/jsontool $(BUILD)/unit $(BUILD)/enginetest \
       $(BUILD)/runtest $(BUILD)/recordtest

$(BUILD)/sg: $(BUILD)/stimgen/main.o $(SG_OBJ)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/stimgen/main.o $(SG_OBJ) $(LDLIBS)

LIBOBJ = $(CORE_OBJ)

$(BUILD)/stimdump: tests/stimdump.c $(LIBOBJ) $(SG_OBJ)
	$(CC) $(CFLAGS) $(INIFLAGS) -Isrc -Ivendor/inih -o $@ tests/stimdump.c $(LIBOBJ) $(SG_OBJ) $(LDLIBS)

$(BUILD)/unit: tests/unit.c $(LIBOBJ) $(SG_OBJ)
	$(CC) $(CFLAGS) $(INIFLAGS) -Isrc -Ivendor/inih -o $@ tests/unit.c $(LIBOBJ) $(SG_OBJ) $(LDLIBS)

$(BUILD)/enginetest: tests/enginetest.c $(LIBOBJ) $(SG_OBJ)
	$(CC) $(CFLAGS) $(INIFLAGS) -Isrc -Ivendor/inih -o $@ tests/enginetest.c $(LIBOBJ) $(SG_OBJ) $(LDLIBS)

$(BUILD)/runtest: tests/runtest.c $(CORE_OBJ) $(BUILD)/record.o $(BUILD)/run.o $(SG_OBJ)
	$(H5CC) $(CFLAGS) -Isrc -o $@ tests/runtest.c $(CORE_OBJ) $(BUILD)/record.o $(BUILD)/run.o $(SG_OBJ) $(LDLIBS)

$(BUILD)/recordtest: tests/recordtest.c $(BUILD)/record.o $(CORE_OBJ) $(SG_OBJ)
	$(H5CC) $(CFLAGS) -Isrc -o $@ tests/recordtest.c $(BUILD)/record.o $(CORE_OBJ) $(SG_OBJ) $(LDLIBS)

$(BUILD)/jsontool: tests/jsontool.c $(BUILD)/json.o $(BUILD)/err.o
	$(CC) $(CFLAGS) -Isrc -o $@ tests/jsontool.c $(BUILD)/json.o $(BUILD)/err.o $(LDLIBS)

# ---------------------------------------------------------------------------
# The pin: the library sources must be the ones LCG2 was tested against.
# STIMGEN_PIN holds the SHA-256 of util.c help.c ... sg_api.c sg.h joined in
# this order.  A different StimGen is noticed here, not silently used.
# ---------------------------------------------------------------------------
check-hdf5:
	@if [ -z "$(H5CC)" ]; then \
	  echo "check-hdf5: HDF5 was not found (no h5cc on the PATH or in a Homebrew keg)."; \
	  echo "  Install it: brew install hdf5   or   sudo apt install libhdf5-dev"; \
	  echo "  or tell make where its compiler wrapper is: make H5CC=/path/to/h5cc"; \
	  exit 1; \
	fi; echo "check-hdf5: using $(H5CC)"

check-pin:
	@want=`sed -n 's/^sources_sha256 *= *//p' STIMGEN_PIN`; \
	files=""; for f in $(SG_FILES); do files="$$files $(STIMGEN_SRC)/$$f.c"; done; \
	files="$$files $(STIMGEN_SRC)/sg.h"; \
	for f in $$files; do [ -r $$f ] || { echo "check-pin: cannot read $$f (is STIMGEN_SRC=$(STIMGEN_SRC) right?)"; exit 1; }; done; \
	if command -v shasum >/dev/null 2>&1; then got=`cat $$files | shasum -a 256 | cut -d' ' -f1`; \
	else got=`cat $$files | sha256sum | cut -d' ' -f1`; fi; \
	if [ "$$got" != "$$want" ]; then \
	  echo "check-pin: the StimGen sources in $(STIMGEN_SRC) differ from STIMGEN_PIN"; \
	  echo "  pinned:   $$want"; echo "  found:    $$got"; \
	  echo "  LCG2 was built and tested against the pinned version; update STIMGEN_PIN only after re-running the tests."; \
	  exit 1; \
	fi; \
	echo "check-pin: StimGen sources match the pin"

test: all
	$(BUILD)/unit
	python3 tests/run_tests.py ./lcg2
	LCG2_BUILD=$(BUILD) python3 tests/test_stim.py
	LCG2_BUILD=$(BUILD) python3 tests/test_engine.py
	LCG2_BUILD=$(BUILD) python3 tests/test_record.py
	python3 tests/test_docs.py ./lcg2
	python3 tests/check_clean.py

sanitize:
	bash tests/sanitize.sh

clean:
	@test -n "$(BUILD)" && test "$(BUILD)" != "/" && test "$(BUILD)" != "."
	rm -rf $(BUILD) lcg2

.PHONY: all tools stimgen-lib check-pin check-hdf5 test sanitize clean
