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
#   make              check the StimGen pin, build ./lcg2
#   make test         run the tests (needs python3)
#   make check-pin    only compare the StimGen sources with STIMGEN_PIN
#   make clean        remove build/ and ./lcg2
#
# Variables you may set on the command line:
#   CC            the C compiler (default cc)
#   STIMGEN_SRC   where the StimGen 2 library sources are (default ../StimGen/src)
#
# StimGen is NOT copied into this project.  Its library files are compiled from
# STIMGEN_SRC into build/, with LCG2's own flags, and only after the sources
# have been compared with the fingerprint in STIMGEN_PIN.  Nothing is ever
# written inside the StimGen folder (it has stale .o files of its own; they
# are not used).  The library is not linked into lcg2 yet (that is milestone
# M2), but it is compiled now so that the flags and the warnings are proved.

CC     ?= cc
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

SRC     = err units config cli main
OBJ     = $(SRC:%=build/%.o) build/ini.o
SG_OBJ  = $(SG_FILES:%=build/stimgen/%.o)

all: check-pin lcg2

lcg2: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.c src/lcg2.h src/units.h src/config.h src/cli.h | build
	$(CC) $(CFLAGS) $(INIFLAGS) -Isrc -Ivendor/inih -c -o $@ $<

build/ini.o: vendor/inih/ini.c vendor/inih/ini.h | build
	$(CC) $(CFLAGS) $(INIFLAGS) -c -o $@ $<

# The StimGen library, compiled into build/stimgen/ (used from M2 on).
stimgen-lib: check-pin $(SG_OBJ)

build/stimgen/%.o: $(STIMGEN_SRC)/%.c $(STIMGEN_SRC)/sg.h | build/stimgen
	$(CC) $(CFLAGS) -I$(STIMGEN_SRC) -c -o $@ $<

build build/stimgen:
	mkdir -p $@

# ---------------------------------------------------------------------------
# The pin: the library sources must be the ones LCG2 was tested against.
# STIMGEN_PIN holds the SHA-256 of util.c help.c ... sg_api.c sg.h joined in
# this order.  A different StimGen is noticed here, not silently used.
# ---------------------------------------------------------------------------
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
	python3 tests/run_tests.py ./lcg2
	python3 tests/test_docs.py ./lcg2

clean:
	rm -rf build lcg2

.PHONY: all stimgen-lib check-pin test clean
