#!/usr/bin/env python3
# LCG2 -- SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michele Giugliano
#
# The last step of `make test`: the tests must not leave anything in the repository.
#
#     python3 tests/check_clean.py
#
# Every real run of lcg2 writes a data file (STEM.h5) and a hidden run folder (.STEM/)
# into the folder it is run in. The tests run real programs in scratch folders; this
# fails if a data file or a run folder ever turns up inside the repository, wherever
# the test that made it was run. (.gitignore also hides them, so that git would not
# notice: this check does.)
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_FOLDER = re.compile(r"^\.(?:.+_)?\d{8}_\d{6}(?:-\d+)?$")        # .STEM and .PREFIX_STEM
found = []
for here, dirs, files in os.walk(ROOT):
    dirs[:] = [d for d in dirs if d not in (".git", "build")]
    for d in list(dirs):
        if RUN_FOLDER.match(d):
            found.append(os.path.join(here, d) + "/")
    for f in files:
        if f.endswith(".h5") or f.endswith(".tmp"):
            found.append(os.path.join(here, f))
if found:
    print("FAIL  the tests left files in the repository (they must run in scratch folders):")
    for f in found:
        print("      " + os.path.relpath(f, ROOT))
    sys.exit(1)
print("repository clean: no data file or run folder was left by the tests")
