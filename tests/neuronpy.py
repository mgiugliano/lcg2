# LCG2 -- SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michele Giugliano
#
# Finding a Python that has NEURON, for the tests that run the example rigs.  The examples say `sim_python = python3`;
# the tests give lcg2 the Python they found through LCG2_SIM_PYTHON, which overrides the file (so the same files work
# on every machine).  Nothing is found -> None, and the tests skip the neuron examples with a note.
import os
import shutil
import subprocess
import sys


def has_neuron(py):
    """True if the Python at this path exists and can import neuron.  -I: do not look in the current folder, where this
    project's own neuron/ folder would be taken for the package."""
    try:
        return subprocess.run([py, "-I", "-c", "import neuron"], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
    except OSError:
        return False


def find():
    cands = [os.environ.get("LCG2_SIM_PYTHON"), os.environ.get("LCG2_NEURON_PYTHON"), sys.executable,
             "/opt/homebrew/opt/python@3.11/bin/python3.11", "/opt/homebrew/opt/python@3.12/bin/python3.12"]
    cands += [shutil.which(n) for n in ("python3", "python3.11", "python3.12", "python3.10")]
    for c in cands:
        if c and has_neuron(c):
            return c
    return None


PY = None if os.environ.get("LCG2_NO_NEURON") else find()


def env():
    e = dict(os.environ)
    if PY:
        e["LCG2_SIM_PYTHON"] = PY
    return e
