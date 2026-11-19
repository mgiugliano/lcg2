#!/usr/bin/env python3
# LCG2 -- SPDX-License-Identifier: MIT -- Copyright (c) 2026 Michele Giugliano
"""
lcg2_neuron.py -- the NEURON side of LCG2 (device = neuron).

You do not run this file yourself.  `lcg2` starts it, once per run, and talks to it
through a pipe; it builds the virtual cells that the configuration describes
([cell NAME] and [synapse NAME] sections), plays the stimulus into them, and sends
back what the electrodes would have measured.  Nobody needs to know NEURON to use it.

To check a model without running an experiment (this is also what `lcg2 -n` does):

    python3 lcg2_neuron.py --check my_rig.ini

THE PIPE PROTOCOL (private to LCG2; the version is PROTOCOL).  Every message from lcg2 is
one line of JSON, optionally followed by raw 8-byte floats (native byte order, same machine);
every reply is one line of JSON, optionally followed by raw floats.

    init     {"op":"init", "prep":<INI text>, "dir":<folder of the config file>, "outs":[{"name","dim"}],
              "ins":[{"name","dim"}], "dt_ms", "celsius", "v_init", "settle_ms", "check"}
                                              -> {"ok":true, "neuron", "cells":[...], "synapses":[...], "files":[...]}
    advance  {"op":"advance", "dur_ns", "cmd":[one number per output]}       -> {"ok":true}
    trial    {"op":"trial", "rate_p", "rate_q"}                                -> {"ok":true}
    block    {"op":"block", "count"} + count floats per output (output-major)   -> {"ok":true} + count floats per input
    quit     {"op":"quit"}

Units on the pipe: current in pA, voltage in mV, time in ns.  An output whose dim is "current"
drives an IClamp (current clamp); "voltage" drives a SEClamp (voltage clamp).  An input whose dim
is "voltage" records the membrane potential where the electrode sits; "current" records the
current the electrode injects.
"""
import array
import hashlib
import importlib.util
import inspect
import json
import math
import os
import random
import re
import shutil
import subprocess
import sys
import traceback
import types

PROTOCOL = 1
BUILTIN_MODELS = ("hh_soma", "ball_and_stick")
BUILTIN_BIOPHYSICS = ("passive", "hh_soma")


class Problem(Exception):
    """A mistake in the configuration or the model files, worded for the person who made it."""


def log(*a):
    print("lcg2_neuron:", *a, file=sys.stderr, flush=True)


# --------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------

_NUM = re.compile(r"^\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)\s*([A-Za-zµ]*)\s*$")


def number(text, what, units=None):
    """A number with an optional unit; `units` maps a suffix to its factor into the base unit."""
    m = _NUM.match(str(text))
    if not m:
        raise Problem("%s: '%s' is not a number%s" % (what, text, " (units: %s)" % ", ".join(units) if units else ""))
    x, u = float(m.group(1)), m.group(2)
    if u:
        if not units or u not in units:
            raise Problem("%s: unknown unit '%s'%s" % (what, u, " (use %s)" % ", ".join(units) if units else " (give a plain number)"))
        x *= units[u]
    return x


MS = {"ms": 1.0, "s": 1000.0, "us": 0.001}
US = {"uS": 1.0, "nS": 0.001, "mS": 1000.0, "S": 1e6}


def autotype(text):
    t = str(text).strip()
    if t.lower() in ("yes", "true"):
        return True
    if t.lower() in ("no", "false"):
        return False
    try:
        return int(t)
    except ValueError:
        pass
    try:
        return float(t)
    except ValueError:
        return t


def parse_ini(text):
    """Sections of the INI text as {"cell P1": {key: value}} in file order (inline ';' comments are dropped)."""
    out, cur = {}, None
    for raw in text.splitlines():
        line = raw.split(";", 1)[0].strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            cur = " ".join(line[1:-1].split())
            out.setdefault(cur, {})
        elif "=" in line and cur is not None:
            k, v = line.split("=", 1)
            out[cur][k.strip()] = v.strip()
    return out


def resolve(path, base, what):
    p = os.path.expanduser(path)
    if not os.path.isabs(p):
        p = os.path.join(base, p)
    p = os.path.normpath(p)
    if not os.path.exists(p):
        raise Problem("%s: the file or folder '%s' does not exist (looked for %s; relative paths start in the folder "
                      "of the configuration file)" % (what, path, p))
    return p


# --------------------------------------------------------------------------
# NEURON loading
# --------------------------------------------------------------------------

def load_neuron():
    try:
        from neuron import h
    except Exception as e:
        raise Problem("cannot import NEURON in %s (%s). Point sim_python in [rig] (or the environment variable LCG2_SIM_PYTHON) at a Python "
                      "that has NEURON (pip install neuron); on the development Mac that is "
                      "/opt/homebrew/opt/python@3.11/bin/python3.11" % (sys.executable, e))
    h.load_file("stdrun.hoc")
    return h


def find_nrnivmodl():
    cands = [shutil.which("nrnivmodl"), os.path.join(os.path.dirname(sys.executable), "nrnivmodl")]
    for c in cands:
        if c and os.path.exists(c):
            return c
    return None


def load_mechanisms(h, folder, loaded):
    """Compile (once, in a cache folder, never in the user's folder) and load a folder of .mod files."""
    if folder in loaded:
        return
    mods = sorted(f for f in os.listdir(folder) if f.endswith(".mod"))
    if not mods:
        raise Problem("mechanisms: the folder %s has no .mod files" % folder)
    dig = hashlib.sha256()
    for f in mods:
        dig.update(f.encode())
        with open(os.path.join(folder, f), "rb") as fh:
            dig.update(fh.read())
    dig.update((sys.platform + os.uname().machine + h.nrnversion(0)).encode())
    root = os.environ.get("LCG2_CACHE") or os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "lcg2")
    work = os.path.join(root, "neuron", dig.hexdigest()[:24])
    lib = None

    def find():
        for dp, _dn, fn in os.walk(work):
            for f in fn:
                if f in ("libnrnmech.dylib", "libnrnmech.so"):
                    return os.path.join(dp, f)
        return None

    lib = find() if os.path.isdir(work) else None
    if lib is None:
        exe = find_nrnivmodl()
        if not exe:
            raise Problem("mechanisms: the .mod files in %s need compiling and nrnivmodl was not found (it comes with "
                          "NEURON; is the NEURON of sim_python complete?)" % folder)
        os.makedirs(os.path.join(work, "mod"), exist_ok=True)
        for f in mods:
            shutil.copy(os.path.join(folder, f), os.path.join(work, "mod", f))
        log("compiling %d .mod file(s) from %s (first time only; cached in %s)" % (len(mods), folder, work))
        r = subprocess.run([exe, "mod"], cwd=work, stdout=sys.stderr, stderr=sys.stderr)
        lib = find()
        if r.returncode != 0 or lib is None:
            raise Problem("mechanisms: nrnivmodl failed on the .mod files in %s (its messages are in the NEURON log of "
                          "the run folder)" % folder)
    if not h.nrn_load_dll(lib):
        raise Problem("mechanisms: NEURON could not load %s" % lib)
    loaded.add(folder)


# --------------------------------------------------------------------------
# cells
# --------------------------------------------------------------------------

class CellObj(object):
    """The Python object that holds the sections of a built-in cell."""

    def __init__(self, name):
        self._name = name

    def __str__(self):
        return self._name


def new_sections(h, before):
    return [s for s in h.allsec() if s.hoc_internal_name() not in before]


def snapshot(h):
    return set(s.hoc_internal_name() for s in h.allsec())


def build_hh_soma(h, name, p):
    known = dict(diam=20.0, length=20.0, cm=1.0, ra=35.4, gnabar=0.12, gkbar=0.036, gl=0.0003, el=-54.3)
    check_params(name, "hh_soma", p, known)
    k = dict(known)
    k.update(p)
    c = CellObj(name)
    c.soma = h.Section(name="soma", cell=c)
    c.soma.L, c.soma.diam, c.soma.cm, c.soma.Ra = k["length"], k["diam"], k["cm"], k["ra"]
    c.soma.nseg = 1
    c.soma.insert("hh")
    for seg in c.soma:
        seg.hh.gnabar, seg.hh.gkbar, seg.hh.gl, seg.hh.el = k["gnabar"], k["gkbar"], k["gl"], k["el"]
    return c


def build_ball_and_stick(h, name, p):
    known = dict(diam=20.0, length=20.0, cm=1.0, ra=35.4, gnabar=0.12, gkbar=0.036, gl=0.0003, el=-54.3,
                 dend_length=200.0, dend_diam=1.0, dend_nseg=11, dend_ra=100.0, dend_g=0.0001, dend_e=-65.0)
    check_params(name, "ball_and_stick", p, known)
    k = dict(known)
    k.update(p)
    c = build_hh_soma(h, name, {a: b for a, b in p.items() if not a.startswith("dend_")})
    c.dend = [h.Section(name="dend", cell=c)]
    d = c.dend[0]
    d.L, d.diam, d.nseg, d.Ra, d.cm = k["dend_length"], k["dend_diam"], int(k["dend_nseg"]), k["dend_ra"], k["cm"]
    d.insert("pas")
    for seg in d:
        seg.pas.g, seg.pas.e = k["dend_g"], k["dend_e"]
    d.connect(c.soma(1))
    return c


def check_params(cell, model, given, known):
    for key in given:
        if key not in known:
            raise Problem("[cell %s]: '%s' is not a parameter of the model %s (its parameters: %s)"
                          % (cell, key, model, ", ".join(sorted(known))))


def load_py(path, tag):
    spec = importlib.util.spec_from_file_location("lcg2_user_%s" % tag, path)
    mod = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(mod)
    except Problem:
        raise
    except Exception as e:
        raise Problem("%s: running the file failed: %s: %s (the full traceback is in the NEURON log of the run folder)"
                      % (path, type(e).__name__, e))
    return mod


def call_filtered(fn, what, args, params):
    """Call fn(*args, **params) passing only the parameters it accepts; a parameter nobody accepts is an error."""
    sig = inspect.signature(fn)
    if any(p.kind == p.VAR_KEYWORD for p in sig.parameters.values()):
        kw = dict(params)
    else:
        accepted = [n for n, p in sig.parameters.items() if p.kind in (p.POSITIONAL_OR_KEYWORD, p.KEYWORD_ONLY)]
        kw = {k: v for k, v in params.items() if k in accepted}
        extra = sorted(set(params) - set(kw))
        if extra:
            raise Problem("%s: '%s' is not a parameter of %s (it takes: %s)" % (what, extra[0], fn.__name__, ", ".join(accepted)))
    try:
        return fn(*args, **kw)
    except Problem:
        raise
    except Exception as e:
        traceback.print_exc()
        raise Problem("%s: %s() failed: %s: %s" % (what, fn.__name__, type(e).__name__, e))


def as_cell(obj, what):
    if obj is None:
        raise Problem("%s: the model returned nothing; it must return the cell (an object with the sections as "
                      "attributes, e.g. cell.soma)" % what)
    if isinstance(obj, dict):
        return types.SimpleNamespace(**obj)
    if type(obj).__name__ == "Section":
        return types.SimpleNamespace(soma=obj)
    return obj


BLANK_TEMPLATE = """
begintemplate LcgBlankCell
public soma, dend, apic, axon, all, somatic, basal, apical, axonal
create soma[1], dend[1], apic[1], axon[1]
objref all, somatic, basal, apical, axonal
proc init() { all = new SectionList() somatic = new SectionList() basal = new SectionList()
              apical = new SectionList() axonal = new SectionList() }
endtemplate LcgBlankCell
"""


def build_morphology(h, name, path):
    h.load_file("import3d.hoc")
    if path.lower().endswith(".swc"):
        reader = h.Import3d_SWC_read()
    elif path.lower().endswith(".asc"):
        reader = h.Import3d_Neurolucida3()
    else:
        raise Problem("[cell %s] morphology: '%s' is not an .swc or .asc file" % (name, path))
    if not hasattr(h, "LcgBlankCell"):
        h(BLANK_TEMPLATE)
    reader.input(path)
    blank = h.LcgBlankCell()
    gui = h.Import3d_GUI(reader, 0)
    gui.instantiate(blank)
    # a Python-friendly view: soma, dend, apic, axon as lists of sections
    c = CellObj(name)
    for part in ("soma", "dend", "apic", "axon"):
        setattr(c, part, [s for s in getattr(blank, part) if s.L > 0 or s.nseg > 0] if hasattr(blank, part) else [])
    c.all = list(blank.all)
    c._hoc = blank
    # the template makes empty placeholder sections; keep only those with geometry
    if not c.soma:
        raise Problem("[cell %s] morphology: %s has no soma (SWC type 1)" % (name, path))
    c.soma = c.soma[0]
    c.all = [s for s in c.all if s.n3d() > 0]
    for part in ("dend", "apic", "axon"):
        setattr(c, part, [s for s in getattr(c, part) if s.n3d() > 0])
    keep = set(s.hoc_internal_name() for s in c.all)
    for s in list(h.allsec()):                      # the template's empty placeholders
        if s.hoc_internal_name().startswith(str(blank)) and s.hoc_internal_name() not in keep:
            h.delete_section(sec=s)
    return c


def apply_d_lambda(h, cell_sections, d_lambda, freq=100.0):
    if d_lambda <= 0:
        return
    for sec in cell_sections:
        lam = 1e5 * math.sqrt(sec.diam / (4 * math.pi * freq * sec.Ra * sec.cm))
        sec.nseg = int((sec.L / (d_lambda * lam) + 0.999) / 2) * 2 + 1


def builtin_biophysics(h, which, cell, sections):
    if which not in BUILTIN_BIOPHYSICS:
        raise Problem("biophysics: '%s' is not a built-in (%s) and not a file" % (which, ", ".join(BUILTIN_BIOPHYSICS)))
    for sec in sections:
        sec.Ra, sec.cm = 35.4 if which == "hh_soma" and sec == cell.soma else 100.0, 1.0
        if which == "hh_soma" and sec == cell.soma:
            sec.insert("hh")
        else:
            sec.insert("pas")
            for seg in sec:
                seg.pas.g, seg.pas.e = 0.0001, -65.0


SECTION_RE = re.compile(r"^\s*([A-Za-z_]\w*)\s*(?:\[\s*(\d+)\s*\])?\s*(?:\(\s*([0-9.]+)\s*\))?\s*$")


def locate(cell, text, what):
    """soma(0.5), dend[2](1) ... -> a segment of the cell."""
    m = SECTION_RE.match(text)
    if not m:
        raise Problem("%s: '%s' is not a place on a cell; write for example soma(0.5) or dend[2](1)" % (what, text))
    name, idx, pos = m.group(1), m.group(2), float(m.group(3) or 0.5)
    if not 0.0 <= pos <= 1.0:
        raise Problem("%s: the position %g is not between 0 and 1" % (what, pos))
    try:
        sec = getattr(cell, name)
        if idx is not None or isinstance(sec, (list, tuple)) or (hasattr(sec, "__getitem__") and type(sec).__name__ != "Section"):
            sec = sec[int(idx or 0)]
    except Exception:
        raise Problem("%s: the cell has no section '%s%s'" % (what, name, "[%s]" % idx if idx is not None else ""))
    if type(sec).__name__ != "Section":
        raise Problem("%s: '%s' is not a section" % (what, name))
    return sec(pos), sec


# --------------------------------------------------------------------------
# the network
# --------------------------------------------------------------------------

ELECTRODE_KEYS = ("electrode_resistance", "electrode_capacitance", "recording_noise", "noise_seed", "bridge_balance",
                  "cap_neutralization", "rs_compensation", "cp_compensation", "seal_resistance",
                  "liquid_junction_potential", "amplifier_bandwidth", "rs_lag")
CELL_KEYS = ("model", "template", "morphology", "biophysics", "init", "mechanisms", "electrode", "d_lambda",
             "series_resistance", "same_as") + ELECTRODE_KEYS


def build_cell(h, name, spec, base, loaded_mech):
    """spec: {key: value} of [cell NAME]. Returns (Cell record, files used)."""
    files = []
    what = "[cell %s]" % name
    params = {k: autotype(v) for k, v in spec.items() if k not in CELL_KEYS}
    model = spec.get("model", "")
    morph = spec.get("morphology", "")
    if not model and not morph:
        model = "hh_soma"
    if model and morph:
        raise Problem("%s: give either model or morphology, not both (a morphology is completed with biophysics)" % what)

    if "mechanisms" in spec:
        folder = resolve(spec["mechanisms"], base, what + " mechanisms")
        load_mechanisms(h, folder, loaded_mech)
        files += [os.path.join(folder, f) for f in sorted(os.listdir(folder)) if f.endswith(".mod")]

    before = snapshot(h)
    d_lambda = float(spec.get("d_lambda", 0.1)) if morph else 0.0
    obj = None
    if morph:
        path = resolve(morph, base, what + " morphology")
        files.append(path)
        obj = build_morphology(h, name, path)
        bio = spec.get("biophysics", "hh_soma")
        sections = list(obj.all)
        if bio in BUILTIN_BIOPHYSICS:
            builtin_biophysics(h, bio, obj, sections)
    elif model == "hh_soma":
        obj = build_hh_soma(h, name, params)
    elif model == "ball_and_stick":
        obj = build_ball_and_stick(h, name, params)
    else:
        path = resolve(model, base, what + " model")
        files.append(path)
        if path.endswith(".py"):
            mod = load_py(path, name)
            if hasattr(mod, "build"):
                obj = call_filtered(mod.build, what + " model", (), params)
            elif hasattr(mod, "Cell"):
                obj = call_filtered(mod.Cell, what + " model", (), params)
            else:
                obj = mod                                   # a plain script: the module is the cell
            obj = as_cell(obj, what + " model")
        elif path.endswith(".hoc"):
            if not h.load_file(path):
                raise Problem("%s model: NEURON could not read %s" % (what, path))
            tmpl = spec.get("template", "")
            if tmpl:
                if not hasattr(h, tmpl):
                    raise Problem("%s: %s does not define a template named '%s'" % (what, path, tmpl))
                args = [v for v in params.values()]
                obj = getattr(h, tmpl)(*args)
            else:
                obj = h                                     # sections created at the top level: h.soma, ...
        else:
            raise Problem("%s model: '%s' is not hh_soma, ball_and_stick, a .py file or a .hoc file" % (what, model))

    sections = list(obj.all) if morph else new_sections(h, before)
    if not sections:
        raise Problem("%s: the model made no sections" % what)

    if "biophysics" in spec and spec["biophysics"] not in BUILTIN_BIOPHYSICS:
        path = resolve(spec["biophysics"], base, what + " biophysics")
        files.append(path)
        run_hook(h, path, "biophysics", obj, sections, params, what, name)
    if morph:                                       # after the biophysics: the rule needs the final Ra and cm
        apply_d_lambda(h, sections, d_lambda)
    init = None
    if "init" in spec:
        init = resolve(spec["init"], base, what + " init")
        files.append(init)
    return dict(name=name, obj=obj, sections=sections, files=files, spec=spec, params=params, init=init)


def run_hook(h, path, hook, obj, sections, params, what, name):
    """Run a user file that adjusts a built cell: Python (a function `hook(cell)` or a plain script that sees
    `cell`, `h`, `sections`) or hoc (a proc `hook` taking the SectionList and the cell)."""
    if path.endswith(".py"):
        spec = importlib.util.spec_from_file_location("lcg2_user_%s_%s" % (hook, name), path)
        mod = importlib.util.module_from_spec(spec)
        mod.cell, mod.h, mod.sections, mod.params = obj, h, sections, params
        try:
            spec.loader.exec_module(mod)
            fn = getattr(mod, hook, None)
            if callable(fn):
                call_filtered(fn, "%s %s" % (what, hook), (obj,), params)
        except Problem:
            raise
        except Exception as e:
            traceback.print_exc()
            raise Problem("%s %s: %s failed: %s: %s" % (what, hook, path, type(e).__name__, e))
    elif path.endswith(".hoc"):
        if not h.load_file(path):
            raise Problem("%s %s: NEURON could not read %s" % (what, hook, path))
        sl = h.SectionList()
        for s in sections:
            sl.append(sec=s)
        if hasattr(h, hook):
            getattr(h, hook)(sl, obj if obj is not h else None)
    else:
        raise Problem("%s %s: '%s' is not a .py or .hoc file" % (what, hook, path))


# --------------------------------------------------------------------------
# electrodes: the pipette, the amplifier, the noise
# --------------------------------------------------------------------------
#
# Units on this side: nA, mV, MOhm, pF, ms (so that nA*MOhm = mV and nA*ms/pF = mV).
#
# Current clamp.  The pipette is a series resistance Re between the amplifier and the cell and a capacitance Ce
# from the amplifier side to ground:
#       (Ce - Cn) dVn/dt = Icmd - (Vn - Vm)/Re          Iin = (Vn - Vm)/Re  flows into the cell
# (nA * ms / pF is 1000 mV, MOhm * pF is a microsecond: the factors of 1000 are in the code).
# The node is solved exactly over each NEURON step (it is linear): pre() injects the mean of Iin over the step
# with Vm held at its value at the start, post() takes Vm as linear over the step to find Vn at its end, so that
# what is read is the end of the step (no lag) whatever the time constant Re*(Ce - Cn).
# The amplifier reports Vn - Rb*Icmd (Rb is the bridge balance, Cn the capacitance neutralisation), plus noise.
# With Re = Ce = Rb = Cn = 0 this is an ideal electrode and the cell sees Icmd exactly.
#
# Voltage clamp.  The clamp has a series resistance Rs (electrode_resistance, or series_resistance) between the
# amplifier and the cell.  Series-resistance compensation is positive feedback: the amplifier adds
# alpha*Rs*I to its command (alpha = rs_compensation/100), through a low-pass of time constant rs_lag, as the
# LAG control of a real amplifier does:  lag dVc/dt = alpha*Rs*I - Vc.  With rs_lag = 0 the compensation is
# ideal: the clamp's series resistance becomes (1 - alpha)*Rs.  The pipette capacitance Cp draws Cp*dVcmd/dt
# (spread over the sample in which the command changes) and the amplifier subtracts Cpc*dVcmd/dt
# (cp_compensation).  The current is reported with noise.
#
# Seal.  A finite seal resistance is a leak from the cell's interior to the bath: a current -(Vm - 0)/Rseal into
# the cell (an extra conductance with a reversal of 0 mV), in both modes.  Liquid junction potential.  The
# recorded potential reads Vm + LJP, and the potential the cell really sits at is the command minus LJP.
# Bandwidth.  What the amplifier reports, its noise included, passes a 4-pole Bessel low-pass (Bessel4) before
# the digitiser.
#
# The step.  The filter and the compensation loop are computed at the NEURON step, so the step is made as small
# as they need (step_needed): sim_dt is the largest step, not always the one used.


class Bessel4(object):
    """A 4-pole Bessel low-pass with its -3 dB point at fc (kHz): the usual output filter of a patch-clamp amplifier.

    Two second-order sections, each made digital with the bilinear transform at the NEURON step (pre-warped to
    its own corner, so that each section is exact there).  It runs at the step of the simulation, not at the
    sampling rate, so that it behaves like the analogue filter that comes before the digitiser.  The error of
    the bilinear transform grows as (fc*dt)^2: within 0.25 percent of the analogue filter from 0 to twice the
    corner when fc*dt <= MAX_FC_DT (17 percent at fc*dt = 0.1), so the step is made that small (step_needed)."""
    POLES = ((-2.8962106, 0.8672341), (-2.1037894, 2.6574180))      # roots of s^4 + 10 s^3 + 45 s^2 + 105 s + 105
    W3DB = 2.1139177                                                 # their -3 dB frequency, rad per unit delay
    MAX_FC_DT = 0.0125

    def __init__(self, fc_khz):
        self.fc = fc_khz
        self.sec = []
        self.dt = None
        self.last = 0.0

    def set_dt(self, dt_ms):
        """(Re)compute the coefficients for a step of dt_ms; the state is kept."""
        if self.dt == dt_ms:
            return
        old = [(x[5], x[6], x[7], x[8]) for x in self.sec]
        self.sec, self.dt = [], dt_ms
        scale = 2.0 * math.pi * self.fc / self.W3DB                  # rad/ms, since a kHz is a 1/ms
        for n, (re, im) in enumerate(self.POLES):
            w0, sg = math.hypot(re, im) * scale, -re * scale
            k = w0 / math.tan(w0 * dt_ms / 2.0)
            a0 = k * k + 2.0 * sg * k + w0 * w0
            b0, b1 = w0 * w0 / a0, 2.0 * w0 * w0 / a0
            a1, a2 = 2.0 * (w0 * w0 - k * k) / a0, (k * k - 2.0 * sg * k + w0 * w0) / a0
            self.sec.append([b0, b1, a1, a2, 0, 0.0, 0.0, 0.0, 0.0])
            if old:
                self.sec[n][5:9] = old[n]
        # indices: 0 b0 (= b2), 1 b1, 2 a1, 3 a2, 5 u[n-1], 6 u[n-2], 7 y[n-1], 8 y[n-2]

    def reset(self, u):
        """Start at rest on the constant input u."""
        for sec in self.sec:
            sec[5] = sec[6] = sec[7] = sec[8] = u
        self.last = u

    def step(self, u):
        for sec in self.sec:
            y = sec[0] * (u + sec[6]) + sec[1] * sec[5] - sec[2] * sec[7] - sec[3] * sec[8]
            sec[6], sec[5] = sec[5], u
            sec[8], sec[7] = sec[7], y
            u = y
        self.last = u
        return u


class Noise(object):
    """White Gaussian noise, reproducible: the stream continues from trial to trial."""

    def __init__(self, sd, seed):
        self.sd = sd
        self.rng = random.Random(seed)

    def __call__(self):
        return self.rng.gauss(0.0, self.sd) if self.sd else 0.0


def electrode_spec(name, spec):
    w = "[cell %s] " % name

    def g(key, unit, units=None, default=0.0):
        x = number(spec.get(key, default), w + key + " (" + unit + ")", units)
        if x < 0:
            raise Problem("%s%s must not be negative (it is %g)" % (w, key, x))
        return x
    R = {"MOhm": 1.0, "kOhm": 0.001, "GOhm": 1000.0}
    C = {"pF": 1.0, "nF": 1000.0}
    e = dict(re=g("electrode_resistance", "MOhm", R), ce=g("electrode_capacitance", "pF", C),
             noise=g("recording_noise", "mV or pA, rms"), rb=g("bridge_balance", "MOhm", R),
             cn=g("cap_neutralization", "pF", C), rsc=g("rs_compensation", "percent"), cpc=g("cp_compensation", "pF", C),
             seed=int(g("noise_seed", "integer", None, 1)), rs=g("series_resistance", "MOhm", R, 1.0),
             seal=1000.0 * g("seal_resistance", "GOhm", {"GOhm": 1.0, "MOhm": 0.001}),            # kept in MOhm
             bw=g("amplifier_bandwidth", "kHz", {"kHz": 1.0, "Hz": 0.001}),
             lag=g("rs_lag", "us", {"us": 0.001, "ms": 1.0}, "10us"),                                # kept in ms
             ljp=number(spec.get("liquid_junction_potential", 0.0), w + "liquid_junction_potential (mV)"))
    if e["cn"] > 0 and e["cn"] >= e["ce"]:
        raise Problem("%scap_neutralization (%g pF) must stay below electrode_capacitance (%g pF): at or above it the "
                      "amplifier would oscillate, as a real one does" % (w, e["cn"], e["ce"]))
    if e["rsc"] >= 100:
        raise Problem("%srs_compensation must be below 100 percent (it is %g): at 100 the loop is unstable" % (w, e["rsc"]))
    if 0 < e["lag"] < 0.001:
        raise Problem("%srs_lag must be 0 (ideal compensation) or at least 1 us (it is %g us)" % (w, e["lag"] * 1000.0))
    if e["bw"] > 100:
        raise Problem("%samplifier_bandwidth must be at most 100 kHz (it is %g kHz), the widest filter of a patch-clamp "
                      "amplifier" % (w, e["bw"]))
    e["model"] = any(e[k] > 0 for k in ("re", "ce", "rb", "cn", "rsc", "cpc", "seal")) or e["ljp"] != 0 or e["bw"] > 0
    e["active"] = e["model"] or e["noise"] > 0
    return e


LAG_STEPS = 20          # steps per rs_lag: the loop is then within ~1 percent of its limit dt -> 0 (tests/test_neuron.py)


def o_dim(outs, name):
    """What the [out NAME] drives ("current" or "voltage"), or None for a pipette that only records."""
    return next((o["dim"] for o in outs if o["name"] == name), None)


def step_needed(name, es, out_dim):
    """The largest NEURON step (ms) at which this pipette is computed accurately, and why (inf, "" if any step)."""
    best, why = float("inf"), ""
    if es["bw"] > 0:
        best, why = Bessel4.MAX_FC_DT / es["bw"], "[cell %s] amplifier_bandwidth %g kHz needs it" % (name, es["bw"])
    if out_dim == "voltage" and es["rsc"] > 0 and es["lag"] > 0 and es["lag"] / LAG_STEPS < best:
        best, why = es["lag"] / LAG_STEPS, "[cell %s] rs_lag %g us needs it" % (name, es["lag"] * 1000.0)
    return best, why


def make_seal(h, rec, es):
    """The seal leak: an extra current into the cell, set at every step (None without a seal)."""
    if es["seal"] <= 0:
        return None
    c = h.IClamp(rec["seg"])
    c.delay, c.dur, c.amp = 0, 1e9, 0
    return c


class CCElectrode(object):
    kind = "i"

    def __init__(self, h, rec, es):
        self.clamp = h.IClamp(rec["seg"])
        self.clamp.delay, self.clamp.dur, self.clamp.amp = 0, 1e9, 0
        self.vref = rec["seg"]._ref_v
        self.re, self.ce, self.cn, self.rb = es["re"], es["ce"], es["cn"], es["rb"]
        self.ljp, self.seal = es["ljp"], es["seal"]
        self.model = es["model"]
        self.leak = make_seal(h, rec, es)
        self.tau = self.re * (self.ce - self.cn) * 1e-3 if self.re > 0 else 0.0      # MOhm*pF is a microsecond: ms
        self.icmd = 0.0
        self.vn = self.vm0 = self.decay = 0.0
        self.start()

    def start(self):
        """The node at rest on the cell as it is now (after finitialize and the init hooks)."""
        self.vn = self.vref[0] + self.re * self.icmd

    def command(self, pa, T_ms=None):
        self.icmd = pa * 1e-3

    def pre(self, dt):
        if not self.model:
            self.clamp.amp = self.icmd
            return
        vm = self.vm0 = self.vref[0]
        if self.leak is not None:
            self.leak.amp = -vm / self.seal
        iin = self.icmd
        if self.tau > 0:
            # with Vm held, Vn relaxes to Vm + Re*Icmd with the time constant tau: inject the mean of Iin
            self.decay = math.exp(-dt / self.tau)
            iin += (self.vn - vm - self.re * self.icmd) * (self.tau / dt) * (1.0 - self.decay) / self.re
        self.clamp.amp = iin

    def coarse(self, on):
        pass

    def post(self, dt):
        if not self.model or self.re <= 0:
            return
        vm = self.vref[0]
        if self.tau > 0:
            # Vm linear over the step (slope s): Vn(t) = Vm(t) + Re*Icmd - tau*s + C*exp(-t/tau)
            s = (vm - self.vm0) / dt
            off = self.re * self.icmd - self.tau * s
            self.vn = vm + off + (self.vn - self.vm0 - off) * self.decay
        else:
            self.vn = vm + self.re * self.icmd

    def read_v(self):
        if not self.model:
            return self.vref[0]
        vn = self.vref[0] if self.re <= 0 else self.vn
        return vn - self.rb * self.icmd + self.ljp

    def read_i(self):
        return self.icmd * 1e3


class VCElectrode(object):
    kind = "v"

    def __init__(self, h, rec, es):
        self.clamp = h.SEClamp(rec["seg"])
        self.clamp.dur1, self.clamp.dur2, self.clamp.dur3, self.clamp.amp1 = 1e9, 0, 0, 0
        self.rs = es["re"] if es["re"] > 0 else es["rs"]
        self.alpha = es["rsc"] / 100.0
        self.lag = es["lag"] if self.alpha > 0 else 0.0
        # ideal compensation (no lag) takes alpha of Rs away; a lagged one is a feedback loop, computed in pre()
        self.clamp.rs = self.rs * (1.0 - self.alpha) if (self.alpha > 0 and self.lag <= 0) else self.rs
        self.cp, self.cpc = es["ce"], es["cpc"]
        self.ljp, self.seal = es["ljp"], es["seal"]
        self.model = es["model"]
        self.leak = make_seal(h, rec, es)
        self.vref = rec["seg"]._ref_v
        self.iref = self.clamp._ref_i
        self.vcmd, self.vprev, self.dv = 0.0, None, 0.0
        self.vc = 0.0                                     # the compensation voltage, mV
        self.uprev, self.paused = None, False

    def start(self):
        self.vc, self.uprev = 0.0, None

    def coarse(self, on):
        """Pauses run at sim_dt, too coarse for a lagged loop: the compensation is ideal meanwhile.  Both have the
        same steady state, so at the end of the pause the loop takes over where the ideal one stands."""
        if self.lag <= 0 or not self.model:
            return
        self.paused = on
        if on:
            self.clamp.rs, self.vc = self.rs * (1.0 - self.alpha), 0.0
        else:
            # the same current I flows through the full Rs once the amplifier adds Vc = alpha*Rs*I
            self.vc = self.alpha * self.rs * self.iref[0]
            self.clamp.rs, self.uprev = self.rs, None

    def command(self, mv, T_ms=None):
        self.dv = 0.0 if (self.vprev is None or not T_ms) else (mv - self.vprev) / T_ms
        self.vprev = mv
        self.vcmd = mv

    def pre(self, dt):
        if not self.model:
            self.clamp.amp1 = self.vcmd
            return
        if self.leak is not None:
            self.leak.amp = -self.vref[0] / self.seal
        if self.lag > 0 and not self.paused:
            # lag dVc/dt = alpha*Rs*I - Vc, solved exactly over the step with I extrapolated linearly from the last
            # two steps (the loop then converges as dt^2, not as dt)
            u0 = self.alpha * self.rs * self.iref[0]
            s = (u0 - self.uprev) / dt if self.uprev is not None else 0.0
            self.uprev = u0
            e = math.exp(-dt / self.lag)
            self.vc = u0 + s * dt - self.lag * s + (self.vc - u0 + self.lag * s) * e
        self.clamp.amp1 = self.vcmd - self.ljp + self.vc

    def post(self, dt):
        pass

    def read_v(self):
        return self.vref[0] + self.ljp

    def read_i(self):
        return self.iref[0] * 1e3 + (self.cp - self.cpc) * self.dv


class Network(object):
    def __init__(self, h, prep, base, outs, ins, opts):
        self.h = h
        self.cells = {}
        self.syns = []
        self.files = []
        self.mech = set()
        sections = parse_ini(prep)
        h.celsius = opts["celsius"]
        names = [s.split(" ", 1)[1] for s in sections if s.startswith("cell ")]
        if not names:
            raise Problem("device = neuron needs at least one [cell NAME] section (for example [cell P1] with "
                          "model = hh_soma)")
        for name in names:
            spec = sections["cell " + name]
            if "same_as" in spec:                       # a second electrode on a cell that already exists
                target = self.cells.get(spec["same_as"])
                if target is None:
                    raise Problem("[cell %s]: same_as = '%s' must name a [cell] written EARLIER in the file (cells: %s)"
                                  % (name, spec["same_as"], ", ".join(self.cells) or "none yet"))
                others = sorted(k for k in spec if k not in ("same_as", "electrode") + ELECTRODE_KEYS)
                if others:
                    raise Problem("[cell %s]: with same_as the cell is that of %s; '%s' belongs to the cell, not to the "
                                  "electrode (only electrode and the electrode_/amplifier keys are allowed here)"
                                  % (name, spec["same_as"], others[0]))
                rec = dict(target)
                rec.update(spec=spec, name=name, files=[], init=None, shared=spec["same_as"])
                self.cells[name] = rec
                continue
            rec = build_cell(h, name, spec, base, self.mech)
            self.cells[name] = rec
            self.files += rec["files"]
        for s in sections:
            if s.startswith("synapse "):
                self.syns.append(self.build_synapse(s.split(" ", 1)[1], sections[s]))
        for name, rec in self.cells.items():
            loc = rec["spec"].get("electrode", "soma(0.5)")
            rec["seg"], rec["sec"] = locate(rec["obj"], loc, "[cell %s] electrode" % name)
            rec["loc"] = loc

        # electrodes
        self.out_e, self.in_e, self.out_el, self.noise, self.in_read = [], [], [], {}, []
        for name, rec in self.cells.items():
            rec["es"] = electrode_spec(name, rec["spec"])
            if rec["es"]["noise"] > 0:
                self.noise[name] = Noise(rec["es"]["noise"], rec["es"]["seed"] + 7919 * len(self.noise))
        # the step: sim_dt, or less where a pipette needs it
        self.dt_max, self.dt_why = opts["dt_ms"], ""
        for name in [o["name"] for o in outs] + [i["name"] for i in ins]:
            rec = self.cells.get(name)
            if rec is not None:
                dt, why = step_needed(name, rec["es"], o_dim(outs, name))
                if dt < self.dt_max:
                    self.dt_max, self.dt_why = dt, why
        if self.dt_why:
            log("the step is %g us instead of sim_dt %g us: %s" % (self.dt_max * 1000.0, opts["dt_ms"] * 1000.0, self.dt_why))
        for rec in self.cells.values():
            rec["es"]["step_ms"] = self.dt_max if self.dt_why else 0.0
        for o in outs:
            rec = self.cells.get(o["name"])
            if rec is None:
                raise Problem("[out %s] has no [cell %s]: with device = neuron every pipette needs a cell" % (o["name"], o["name"]))
            es = rec["es"]
            if es["seal"] > 0:                                    # the leak is coupled explicitly: it must be slow enough
                cseg = rec["seg"].area() * rec["seg"].cm * 0.01
                if es["seal"] * cseg * 1e-3 < opts["dt_ms"]:
                    raise Problem("[cell %s] seal_resistance %g GOhm is too small for sim_dt %g ms with the %.1f pF of "
                                  "membrane under the electrode: use at least %.4f GOhm or a smaller sim_dt"
                                  % (o["name"], es["seal"] / 1000.0, opts["dt_ms"], cseg, opts["dt_ms"] / cseg))
            if o["dim"] == "current":
                el = CCElectrode(h, rec, es)
                if es["re"] > 0 and es["ce"] > 0:               # the electrode is stiff: the explicit coupling must be stable
                    cseg = rec["seg"].area() * rec["seg"].cm * 0.01
                    if es["re"] * cseg * 1e-3 < opts["dt_ms"]:             # Re * C in ms: MOhm * pF is a microsecond
                        raise Problem("[cell %s] electrode_resistance %g MOhm is too small for sim_dt %g ms with the "
                                      "%.1f pF of membrane under the electrode: use at least %.1f MOhm or a smaller sim_dt"
                                      % (o["name"], es["re"], opts["dt_ms"], cseg, opts["dt_ms"] * 1000.0 / cseg))
            else:
                el = VCElectrode(h, rec, es)
            self.out_e.append((el.kind, el.clamp))
            self.out_el.append(el)
            rec["clamp"], rec["el"] = el.clamp, el
        for i in ins:
            rec = self.cells.get(i["name"])
            if rec is None:
                raise Problem("[in %s] has no [cell %s]: with device = neuron every pipette needs a cell" % (i["name"], i["name"]))
            noise = self.noise.get(i["name"])
            el = rec.get("el")
            es = rec["es"]
            if es["seal"] > 0 and el is None:
                raise Problem("[cell %s] seal_resistance needs an [out %s]: the seal is part of an electrode that injects or clamps"
                              % (i["name"], i["name"]))
            if i["dim"] == "voltage":
                self.in_e.append(("v", rec["seg"]._ref_v))
                read = el.read_v if el else (lambda ref=rec["seg"]._ref_v, off=es["ljp"]: ref[0] + off)
            else:
                if el is None:
                    raise Problem("[in %s] records current but there is no [out %s] to inject it" % (i["name"], i["name"]))
                self.in_e.append(("i", el.clamp._ref_amp if el.kind == "i" else el.clamp._ref_i))
                read = el.read_i
            filt = Bessel4(es["bw"]) if es["bw"] > 0 else None            # the step was made small enough for it
            self.in_read.append((read, noise, filt))
        # the fast path (plain clamps, nothing else) is used unless a pipette has something to model (a pipette that
        # only records counts too: its junction potential and filter are applied on the slow path)
        self.model = (any(el.model for el in self.out_el) or bool(self.noise)
                      or any(self.cells[i["name"]]["es"]["model"] for i in ins))
        self.sim_dt = opts["dt_ms"]                     # the largest step, used in pauses (the checks above are made for it)
        self.opts = dict(opts, dt_ms=self.dt_max)       # the step within trials
        self.nsub = 1
        h.dt = self.dt_max
        h.v_init = opts["v_init"]
        h.finitialize(opts["v_init"])
        for rec in self.cells.values():
            if rec["init"]:
                run_hook(h, rec["init"], "initialize", rec["obj"], rec["sections"], rec["params"], "[cell %s]" % rec["name"], rec["name"])
        h.fcurrent()
        for el in self.out_el:                            # the pipettes start at rest on the cell as initialised
            el.start()
        self.advance_ms(opts["settle_ms"], [0.0] * len(self.out_e))
        self.filters_to_rest()

    def build_synapse(self, name, spec):
        h = self.h
        what = "[synapse %s]" % name
        known = ("pre", "post", "type", "weight", "delay", "tau", "tau1", "tau2", "e", "threshold", "pre_location", "post_location")
        for k in spec:
            if k not in known:
                raise Problem("%s: unknown key '%s' (keys: %s)" % (what, k, ", ".join(known)))
        for k in ("pre", "post"):
            if spec.get(k) not in self.cells:
                raise Problem("%s: %s = '%s' is not a [cell] (cells: %s)" % (what, k, spec.get(k, ""), ", ".join(self.cells)))
        typ = spec.get("type", "exp")
        if typ not in ("exp", "exp2"):
            raise Problem("%s: type must be exp or exp2, not '%s'" % (what, typ))
        pre, post = self.cells[spec["pre"]], self.cells[spec["post"]]
        ploc = locate(pre["obj"], spec.get("pre_location", pre["spec"].get("electrode", "soma(0.5)")), what + " pre_location")
        qloc = locate(post["obj"], spec.get("post_location", post["spec"].get("electrode", "soma(0.5)")), what + " post_location")
        e = number(spec.get("e", 0.0), what + " e (mV)")
        if typ == "exp":
            syn = h.ExpSyn(qloc[0])
            syn.tau = number(spec.get("tau", 2.0), what + " tau (ms)", MS)
        else:
            syn = h.Exp2Syn(qloc[0])
            syn.tau1 = number(spec.get("tau1", 0.5), what + " tau1 (ms)", MS)
            syn.tau2 = number(spec.get("tau2", 2.0), what + " tau2 (ms)", MS)
            if syn.tau2 <= syn.tau1:
                raise Problem("%s: tau2 (%g ms) must be larger than tau1 (%g ms)" % (what, syn.tau2, syn.tau1))
        syn.e = e
        nc = h.NetCon(ploc[0]._ref_v, syn, number(spec.get("threshold", 0.0), what + " threshold (mV)"),
                      number(spec.get("delay", 1.0), what + " delay (ms)", MS),
                      number(spec.get("weight", 0.001), what + " weight (uS)", US), sec=ploc[1])
        return dict(name=name, syn=syn, nc=nc, pre=spec["pre"], post=spec["post"], type=typ)

    # -- time ---------------------------------------------------------
    def set_commands(self, cmd):
        if self.model:
            for el, x in zip(self.out_el, cmd):
                el.command(x, None)
            return
        for (kind, c), x in zip(self.out_e, cmd):
            if kind == "i":
                c.amp = x * 1e-3          # pA -> nA
            else:
                c.amp1 = x                # mV

    def filters_to_rest(self):
        """The output filters start (and, after a pause, restart) at rest on what the amplifier reads now."""
        for read, _noise, filt in self.in_read:
            if filt:
                filt.set_dt(self.h.dt)
                filt.reset(read())

    def advance_ms(self, dur_ms, cmd):
        h = self.h
        self.set_commands(cmd)
        if dur_ms <= 0:
            return
        target = h.t + dur_ms
        n = max(1, int(round(dur_ms / h.dt)))
        if self.model:
            els, fadvance, keep = self.out_el, h.fadvance, h.dt
            if self.dt_why:
                # the step was made small for what is recorded; nothing is recorded in a pause: back to sim_dt
                n = max(1, int(math.ceil(dur_ms / self.sim_dt - 1e-9)))
                h.dt = dur_ms / n
                for el in els:
                    el.coarse(True)
            dt = h.dt
            for _ in range(n):
                for el in els:
                    el.pre(dt)
                fadvance()
                for el in els:
                    el.post(dt)
            if self.dt_why:
                for el in els:
                    el.coarse(False)
                h.dt = keep
            self.filters_to_rest()
        else:
            for _ in range(n):
                h.fadvance()
        if abs(h.t - target) > 1e-6 * max(1.0, dur_ms):
            log("warning: advanced to t=%.6f ms instead of %.6f" % (h.t, target))

    def start_trial(self, p, q):
        period_ms = 1000.0 * q / p
        self.nsub = max(1, int(math.ceil(period_ms / self.opts["dt_ms"] - 1e-9)))
        self.h.dt = period_ms / self.nsub
        self.period_ms = period_ms
        for _read, _noise, filt in self.in_read:
            if filt:
                filt.set_dt(self.h.dt)

    def block(self, cmd, count):
        """cmd: per output an array('d') of `count` values (pA or mV). Returns per input an array('d')."""
        h = self.h
        fadvance = h.fadvance
        nsub = self.nsub
        outs, ins = self.out_e, self.in_e
        res = [array.array("d", bytes(8 * count)) for _ in ins]
        if self.model:
            els, reads, dt, T = self.out_el, self.in_read, h.dt, self.period_ms
            # A filtered input gets its noise before the filter, drawn at every step: white noise of the same
            # density as the unfiltered one (sd per step = sd per sample * sqrt(T/dt), and T/dt is nsub).
            boost = math.sqrt(nsub)
            filts = [(f, rd, n, boost) for rd, n, f in reads if f]
            for i in range(count):
                for j, el in enumerate(els):
                    el.command(cmd[j][i], T)
                for _ in range(nsub):
                    for el in els:
                        el.pre(dt)
                    fadvance()
                    for el in els:
                        el.post(dt)
                    for f, rd, n, b in filts:
                        f.step(rd() + n() * b if n else rd())
                for k, (read, noise, filt) in enumerate(reads):
                    res[k][i] = filt.last if filt else read() + (noise() if noise else 0.0)
            return res
        icl = [(c, cmd[j]) for j, (k, c) in enumerate(outs) if k == "i"]
        vcl = [(c, cmd[j]) for j, (k, c) in enumerate(outs) if k == "v"]
        refs = [(r, res[j]) for j, (k, r) in enumerate(ins)]
        for i in range(count):
            for c, a in icl:
                c.amp = a[i] * 1e-3
            for c, a in vcl:
                c.amp1 = a[i]
            for _ in range(nsub):
                fadvance()
            for k, (r, out) in enumerate(refs):
                v = r[0]
                out[i] = v * 1e3 if ins[k][0] == "i" else v     # nA -> pA
        return res


def electrode_note(es):
    """A few words for -n: the pipette model, if there is one."""
    bits = []
    for key, label, unit in (("re", "Re", "MOhm"), ("ce", "Ce", "pF"), ("rb", "bridge", "MOhm"), ("cn", "neutralisation", "pF"),
                             ("rsc", "Rs comp", "%"), ("cpc", "Cp comp", "pF"), ("noise", "noise", "rms"),
                             ("seal", "seal", "MOhm"), ("bw", "bandwidth", "kHz")):
        if es[key] > 0:
            bits.append("%s %g %s" % (label, es[key], unit))
            if key == "rsc":
                bits.append("lag %g us" % (es["lag"] * 1000.0) if es["lag"] > 0 else "no lag")
    if es["ljp"] != 0:
        bits.append("LJP %g mV" % es["ljp"])
    if es.get("step_ms"):
        bits.append("step %g us" % (es["step_ms"] * 1000.0))
    return " [" + ", ".join(bits) + "]" if bits else ""


def describe(net):
    cells = []
    for name, rec in net.cells.items():
        cells.append(dict(name=name, model=("same cell as " + rec["shared"]) if rec.get("shared") else
                          (rec["spec"].get("model") or rec["spec"].get("morphology")),
                          sections=len(rec["sections"]), segments=sum(s.nseg for s in rec["sections"]),
                          electrode=rec["loc"] + electrode_note(rec["es"]), v_rest_mV=round(float(rec["seg"].v), 3)))
    syns = [dict(name=s["name"], pre=s["pre"], post=s["post"], type=s["type"]) for s in net.syns]
    return cells, syns


# --------------------------------------------------------------------------
# the pipe
# --------------------------------------------------------------------------

class Pipe(object):
    def __init__(self):
        # NEURON prints to the C-level stdout; keep the protocol on a private copy of it
        self.out = os.fdopen(os.dup(1), "wb", buffering=0)
        os.dup2(2, 1)
        self.inp = sys.stdin.buffer

    def read(self):
        line = self.inp.readline()
        if not line:
            return None
        return json.loads(line)

    def floats(self, n):
        a = array.array("d")
        data = self.inp.read(8 * n)
        if len(data) != 8 * n:
            raise EOFError("lcg2 closed the pipe in the middle of a message")
        a.frombytes(data)
        return a

    def reply(self, obj, arrays=()):
        buf = (json.dumps(obj) + "\n").encode()
        for a in arrays:
            buf += a.tobytes()
        self.out.write(buf)


def serve():
    pipe = Pipe()
    net = None
    try:
        msg = pipe.read()
        if msg is None:
            return 0
        if msg.get("op") != "init":
            pipe.reply({"ok": False, "error": "internal error: the first message must be init"})
            return 1
        try:
            h = load_neuron()
            net = Network(h, msg["prep"], msg["dir"], msg["outs"], msg["ins"],
                          dict(dt_ms=msg["dt_ms"], celsius=msg["celsius"], v_init=msg["v_init"], settle_ms=msg["settle_ms"]))
            cells, syns = describe(net)
            pipe.reply({"ok": True, "protocol": PROTOCOL, "neuron": h.nrnversion(0), "python": sys.version.split()[0],
                        "cells": cells, "synapses": syns, "files": net.files, "dt_ms": msg["dt_ms"], "step_ms": net.dt_max,
                        "celsius": msg["celsius"]})
        except Problem as e:
            pipe.reply({"ok": False, "error": str(e)})
            return 1
        except Exception as e:
            traceback.print_exc()
            pipe.reply({"ok": False, "error": "unexpected %s while building the network: %s" % (type(e).__name__, e)})
            return 1
        if msg.get("check"):
            return 0
        nout, nin = len(net.out_e), len(net.in_e)
        while True:
            msg = pipe.read()
            if msg is None or msg["op"] == "quit":
                return 0
            try:
                op = msg["op"]
                if op == "advance":
                    net.advance_ms(msg["dur_ns"] / 1e6, msg["cmd"])
                    pipe.reply({"ok": True})
                elif op == "trial":
                    net.start_trial(msg["rate_p"], msg["rate_q"])
                    pipe.reply({"ok": True})
                elif op == "block":
                    n = msg["count"]
                    cmd = [pipe.floats(n) for _ in range(nout)]
                    pipe.reply({"ok": True, "count": n}, net.block(cmd, n))
                else:
                    pipe.reply({"ok": False, "error": "unknown operation '%s'" % op})
            except EOFError:
                return 0
            except Exception as e:
                traceback.print_exc()
                pipe.reply({"ok": False, "error": "%s: %s" % (type(e).__name__, e)})
                return 1
    except BrokenPipeError:
        return 0


def check_file(path):
    """lcg2_neuron.py --check FILE.ini : build the network of a configuration file and say what it is."""
    base = os.path.dirname(os.path.abspath(path))
    with open(path) as f:
        text = f.read()
    secs = parse_ini(text)
    prep = "".join("[%s]\n%s\n" % (s, "".join("%s = %s\n" % kv for kv in secs[s].items())) for s in secs if s.startswith(("cell ", "synapse ")))
    h = load_neuron()
    net = Network(h, prep, base, [], [], dict(dt_ms=0.025, celsius=6.3, v_init=-65.0, settle_ms=0.0))
    cells, syns = describe(net)
    print("NEURON %s: %d cell(s), %d synapse(s): ok" % (h.nrnversion(0), len(cells), len(syns)))
    for c in cells:
        print("  cell %-8s %-24s %3d section(s), %4d segment(s), electrode %s, rests at %.1f mV" %
              (c["name"], c["model"], c["sections"], c["segments"], c["electrode"], c["v_rest_mV"]))
    for s in syns:
        print("  synapse %s: %s -> %s (%s)" % (s["name"], s["pre"], s["post"], s["type"]))
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        try:
            sys.exit(check_file(sys.argv[2]))
        except Problem as e:
            print("lcg2_neuron: %s" % e, file=sys.stderr)
            sys.exit(1)
    if len(sys.argv) > 1:
        print(__doc__)
        sys.exit(2)
    sys.exit(serve())
