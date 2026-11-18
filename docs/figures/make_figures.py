#!/usr/bin/env python3
# LCG2 -- SPDX-License-Identifier: MIT
# Copyright (c) 2026 Michele Giugliano
#
# Draws the figures of the documentation as plain SVG files (no libraries):
#
#     python3 docs/figures/make_figures.py
#
# Edit this script, not the SVGs. Style: solid green outline = built and
# tested; dashed outline = designed, not built yet. Every figure has a white
# background so that it reads the same on light and dark pages.
import os
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))

GREEN, GREEN_BG = "#1b7f3b", "#e6f4ea"
GREY, GREY_BG = "#6b7280", "#f3f4f6"
BLUE, BLUE_BG = "#1d4ed8", "#e8eefc"
AMBER, AMBER_BG = "#b45309", "#fdf2e0"
RED = "#b91c1c"
INK = "#111827"


class Svg:
    def __init__(self, w, h):
        self.w, self.h, self.parts = w, h, []

    def add(self, s):
        self.parts.append(s)

    def text(self, x, y, lines, size=14, anchor="middle", weight="normal", fill=INK):
        if isinstance(lines, str):
            lines = lines.split("\n")
        lh = size * 1.3
        y0 = y - (len(lines) - 1) * lh / 2
        for i, l in enumerate(lines):
            self.add('<text x="%g" y="%g" font-size="%g" text-anchor="%s" '
                     'font-weight="%s" fill="%s" dominant-baseline="central">%s</text>'
                     % (x, y0 + i * lh, size, anchor, weight, fill, escape(l)))

    def box(self, x, y, w, h, lines, kind="built", size=14, title=None):
        stroke, bg = {"built": (GREEN, GREEN_BG), "plan": (GREY, GREY_BG),
                      "blue": (BLUE, BLUE_BG), "amber": (AMBER, AMBER_BG),
                      "wait": (AMBER, AMBER_BG)}[kind]
        dash = ' stroke-dasharray="7 4"' if kind in ("plan", "wait") else ""
        self.add('<rect x="%g" y="%g" width="%g" height="%g" rx="9" fill="%s" '
                 'stroke="%s" stroke-width="2"%s/>' % (x, y, w, h, bg, stroke, dash))
        if title:
            self.text(x + w / 2, y + 17, title, size=size, weight="bold")
            self.text(x + w / 2, y + 17 + (h - 17) / 2, lines, size=size - 1)
        else:
            self.text(x + w / 2, y + h / 2, lines, size=size)

    def arrow(self, x1, y1, x2, y2, color=INK, dash=False, label=None, lx=0, ly=-10):
        d = ' stroke-dasharray="6 4"' if dash else ""
        self.add('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="2"%s '
                 'marker-end="url(#ah)"/>' % (x1, y1, x2, y2, color, d))
        if label:
            self.text((x1 + x2) / 2 + lx, (y1 + y2) / 2 + ly, label, size=12, fill=color)

    def line(self, x1, y1, x2, y2, color=INK, w=2, dash=None):
        d = ' stroke-dasharray="%s"' % dash if dash else ""
        self.add('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="%g"%s/>'
                 % (x1, y1, x2, y2, color, w, d))

    def save(self, name, title, desc):
        out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" '
               'height="%d" font-family="Helvetica, Arial, sans-serif" role="img" '
               'aria-labelledby="t d">' % (self.w, self.h, self.w, self.h),
               '<title id="t">%s</title><desc id="d">%s</desc>' % (escape(title), escape(desc)),
               '<defs><marker id="ah" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" '
               'markerHeight="8" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" '
               'fill="%s"/></marker></defs>' % INK,
               '<rect width="100%" height="100%" fill="#ffffff"/>']
        out += self.parts
        out.append("</svg>")
        with open(os.path.join(HERE, name), "w") as f:
            f.write("\n".join(out) + "\n")


def legend(s, x, y):
    s.box(x, y, 26, 16, "", "built")
    s.text(x + 34, y + 8, "built and tested", size=12, anchor="start")
    s.box(x + 150, y, 26, 16, "", "plan")
    s.text(x + 184, y + 8, "designed, not built yet", size=12, anchor="start")
    s.box(x + 340, y, 26, 16, "", "wait")
    s.text(x + 374, y + 8, "waits for a decision", size=12, anchor="start")


# --------------------------------------------------------------------------
def overview():
    s = Svg(940, 600)
    s.text(470, 24, "LCG2: what goes in, what happens, what comes out", size=17, weight="bold")
    # inputs
    s.box(20, 60, 210, 80, "step.sg, a protocol,\nor -e 'TEXT'", "blue", title="Stimulus (StimGen 2)")
    s.box(20, 170, 210, 80, "[rig] [out P1] [in P1]\n[profile NAME]", "blue", title="Configuration (INI)")
    s.box(20, 280, 210, 80, "-c -P -m -r -s -T\n-t -e -p -n -q -v", "blue", title="Command line")
    # core
    s.add('<rect x="270" y="50" width="390" height="400" rx="12" fill="#ffffff" stroke="%s" '
          'stroke-width="2"/>' % INK)
    s.text(465, 68, "lcg2 (a C99 program)", size=14, weight="bold")
    s.box(290, 85, 350, 70, "command line, INI, profiles, units, checks\ncli.c  config.c  units.c  main.c",
          "built", title="1  Read and check   (M0, M1)")
    s.box(290, 175, 350, 70, "trials as samples, units and every sample checked\nonly sg_api_* of StimGen 2 (stim.c, json.c)",
          "built", title="2  Render with StimGen   (M2)")
    s.box(290, 265, 350, 70, "schedule, rest values, delays, Ctrl-C = outputs to rest",
          "built", title="3  Trial engine   (M3)")
    s.box(290, 355, 350, 70, "one interface: write outputs, read inputs",
          "built", title="4  Backend interface   (M3, provisional)")
    for y in (155, 245, 335):
        s.arrow(465, y, 465, y + 20)
    # backends
    s.box(270, 480, 120, 56, "built-in\nloopback", "built", size=13, title=None)
    s.box(405, 480, 130, 56, "NEURON\nsimulator (M3b)", "plan", size=13)
    s.box(550, 480, 160, 56, "NI USB-6343\n(M6, host undecided)", "wait", size=13)
    s.arrow(330, 425, 330, 480)
    s.arrow(470, 425, 470, 480)
    s.arrow(630, 425, 630, 480, dash=True)
    # output
    s.box(720, 150, 200, 190, "YYYYMMDD_HHMMSS.h5\n(trials, inputs, outputs)\n\n.YYYYMMDD_HHMMSS/\nconfig, command, stimulus,\ntrials, log, SHA-256",
          "plan", title="Run archive   (M4)")
    s.arrow(660, 245, 720, 245)
    for y in (100, 210, 320):
        s.arrow(230, y, 270, y)
    legend(s, 20, 568)
    s.save("fig-overview.svg", "LCG2 overview",
           "Three inputs (stimulus text, configuration file, command line) enter the lcg2 program, "
           "which reads and checks them, renders with StimGen, runs a trial engine, talks to a backend "
           "(loopback, NEURON or the board) and writes a run archive.")


def signal_chain():
    s = Svg(980, 470)
    s.text(490, 24, "From a number in the stimulus to a voltage on the board, and back", size=17, weight="bold")
    s.text(20, 62, "OUTPUT (command)", size=13, anchor="start", weight="bold", fill=BLUE)
    w, h, y = 134, 74, 85
    xs = [20, 175, 330, 485, 640]
    items = [
        ("stimulus value", "e.g. 200 pA\n(channel unit)", "blue"),
        ("divide by gain", "400 pA per volt\n= 0.5 V wanted", "built"),
        ("limit check", "|wanted| <= limit\n(protects amplifier)", "amber"),
        ("inverse calibration", "(wanted - cal_offset)\n/ cal_scale = written", "built"),
        ("board check", "|written| <= 10 V\n(board range)", "amber"),
    ]
    for x, (t, b, k) in zip(xs, items):
        s.box(x, y, w, h, b, k, size=13, title=t)
    for a, b in zip(xs, xs[1:]):
        s.arrow(a + w, y + h / 2, b, y + h / 2)
    s.box(795, y, 165, h, "analog output pin\nao0 .. ao3", "plan", size=13, title="board AO line")
    s.arrow(xs[-1] + w, y + h / 2, 795, y + h / 2)
    # cell
    s.box(795, 190, 165, 62, "real cell, or the\nsimulated one", "plan", size=13, title="amplifier + cell")
    s.arrow(877, y + h, 877, 190)
    # input chain
    s.text(20, 262, "INPUT (recording)", size=13, anchor="start", weight="bold", fill=GREEN)
    y2 = 295
    items2 = [
        ("analog input pin", "ai0 .. ai15\nrange, terminal", "plan"),
        ("volts read", "a number per sample\nper channel", "blue"),
        ("calibration", "cal_scale * v\n+ cal_offset", "built"),
        ("multiply by gain", "100 mV per volt", "built"),
        ("recorded value", "mV or pA into HDF5\n(physical units)", "blue"),
    ]
    xs2 = [795 - 0, 640, 485, 330, 175]
    for x, (t, b, k) in zip(xs2, items2):
        s.box(x, y2, w if x != 795 else 165, h, b, k, size=13, title=t)
    s.arrow(877, 252, 877, y2)
    s.arrow(795, y2 + h / 2, 640 + w, y2 + h / 2)
    for a, b in zip(xs2[1:], xs2[2:]):
        s.arrow(a, y2 + h / 2, b + w, y2 + h / 2)
    s.text(490, 412, "Gain = channel units per volt, one number per channel and mode (cc or vc). "
                     "Every limit is checked before anything is played; a sample is never clipped silently.",
           size=12)
    s.text(490, 434, "Green boxes are computed by LCG2; blue are values the user sees; amber are safety checks.",
           size=12, fill=GREY)
    s.save("fig-signal-chain.svg", "Signal chain",
           "Output path: stimulus value, divide by gain, limit check, inverse calibration, board range check, "
           "board output. Input path: board input, volts read, calibration, multiply by gain, recorded value.")


def layers():
    s = Svg(940, 420)
    s.text(470, 24, "How the configuration is assembled (built in M1)", size=17, weight="bold")
    xs = [20, 250, 480, 710]
    names = [("1  lcg2.ini", "every section and key\nsyntax checked\nduplicates refused", "built"),
             ("2  -P NAME", "[profile NAME] lines\nout.P2.line = ao1\noverwrite single values", "built"),
             ("3  -m cc or vc", "overrides rig.mode;\nrecorded as\n'set on the command line'", "built"),
             ("4  typed Config", "units, ranges, lines,\nlimits, rest values\nall checked", "built")]
    for x, (t, b, k) in zip(xs, names):
        s.box(x, 55, 210, 105, b, k, title=t)
    for a, b in zip(xs, xs[1:]):
        s.arrow(a + 210, 107, b, 107)
    s.text(470, 190, "The table after step 3 is printed by  lcg2 -n -v  and is what the run archive will store.",
           size=13)
    # precedence stacks
    s.text(20, 235, "Precedence (strongest first)", size=14, anchor="start", weight="bold")
    s.box(20, 255, 290, 120, "1  rate written in the stimulus\n2  -r (a different value is an error)\n3  [rig] rate in the configuration",
          "built", size=13, title="Sampling rate")
    s.box(325, 255, 290, 120, "1  -s on the command line\n2  seed written in the stimulus\n3  drawn at the start and recorded",
          "built", size=13, title="Seed")
    s.box(630, 255, 290, 120, "1  rest in the stimulus (named channels)\n2  rest in [out] (idle outputs)\n3  0 (digital: the stimulus value, else 0)",
          "built", size=13, title="Rest value")
    s.text(470, 400, "Rate and seed are combined with the stimulus in M2; today -n shows the command-line and file values.",
           size=12, fill=GREY)
    s.save("fig-config-layers.svg", "Configuration layers",
           "The INI file is read, a profile overwrites single values, the -m flag overrides the mode, "
           "and the result is a typed, checked configuration. Precedence rules for rate, seed and rest value are shown.")


def timeline():
    s = Svg(940, 440)
    s.text(470, 24, "Episodic recording: one block per trial, outputs at rest in between", size=17, weight="bold")
    x0, ppx = 120, 38.0  # time axis: 38 px per second, 20 s across

    def scenario(y, label, trials, a_from, a_to, note):
        s.text(20, y, label, size=14, anchor="start", weight="bold")
        s.arrow(x0 + a_from * ppx, y + 26, x0 + a_to * ppx, y + 26, color=AMBER)
        s.text(x0 + a_to * ppx + 10, y + 26, note, size=12, anchor="start", fill=AMBER)
        s.text(20, y + 60, "outputs", size=12, anchor="start", fill=BLUE)
        s.text(20, y + 92, "recorded", size=12, anchor="start", fill=GREEN)
        s.line(x0, y + 60, x0 + 20 * ppx, y + 60, color=GREY, w=1.5, dash="5 4")   # rest value
        for (a, b) in trials:
            xa, xb = x0 + a * ppx, x0 + b * ppx
            s.add('<rect x="%g" y="%g" width="%g" height="24" rx="3" fill="%s" stroke="%s" stroke-width="1.5"/>'
                  % (xa, y + 48, xb - xa, BLUE_BG, BLUE))
            s.add('<rect x="%g" y="%g" width="%g" height="24" rx="3" fill="%s" stroke="%s" stroke-width="1.5"/>'
                  % (xa, y + 80, xb - xa, GREEN_BG, GREEN))

    scenario(60, "period 5s   (start to start)", [(0, 2), (5, 7), (10, 12), (15, 17)], 0, 5,
             "5 s from one start to the next")
    scenario(205, "gap 2s   (end to start)", [(0, 2), (4, 6), (8, 10), (12, 14)], 2, 4,
             "2 s from one end to the next start")
    for t in range(0, 21, 5):
        s.line(x0 + t * ppx, 335, x0 + t * ppx, 341, w=1.5)
        s.text(x0 + t * ppx, 353, "%d s" % t, size=11)
    s.line(x0, 335, x0 + 20 * ppx, 335, w=1.5)
    s.text(20, 335, "time", size=12, anchor="start")
    s.text(470, 385, "Between trials the outputs hold their rest value (dashed line); nothing is recorded.", size=13)
    s.text(470, 407, "A trial that cannot start on time (previous one still running, USB start-up) starts as soon as it can;", size=13)
    s.text(470, 427, "the real start time and delay of every trial are recorded and printed.  (Built, M3; the data file is M4.)",
           size=13, fill=GREY)
    s.save("fig-timeline.svg", "Trial timing",
           "Two timing modes of a protocol. With period, trials start at fixed intervals; with gap, each trial starts a fixed "
           "time after the previous one ended. Outputs rest between trials and inputs are recorded only during trials.")


def multipatch():
    s = Svg(940, 400)
    s.text(470, 24, "Multi-patch: drive one pipette, record all of them", size=17, weight="bold")
    s.box(330, 55, 290, 290, "", "plan")
    s.text(475, 75, "acquisition board", size=14, weight="bold")
    s.box(350, 95, 250, 44, "ao0  =  [out P1]  (driven)", "blue", size=13)
    s.box(350, 150, 250, 44, "ao1  =  [out P2]  (idle, at rest)", "plan", size=13)
    s.box(350, 225, 250, 44, "ai0  =  [in P1]  (recorded)", "built", size=13)
    s.box(350, 280, 250, 44, "ai1  =  [in P2]  (recorded)", "built", size=13)
    s.box(710, 90, 210, 100, "cell P1\ncurrent is injected", "blue", size=13)
    s.box(710, 225, 210, 100, "cell P2\nonly listened to", "plan", size=13)
    s.arrow(600, 117, 710, 117, color=BLUE)
    s.arrow(600, 172, 710, 262, color=GREY, dash=True)
    s.arrow(710, 150, 600, 247, color=GREEN)
    s.arrow(710, 300, 600, 302, color=GREEN)
    s.box(20, 90, 270, 150, "channel P1 unit=pA {\n    1s dc(0)\n    500ms pulses(...)\n    1s dc(0)\n}",
          "blue", size=13, title="the stimulus names only P1")
    s.arrow(290, 117, 350, 117, color=BLUE)
    s.text(155, 280, "A channel without an [out] section is an error.\nAn [out] the stimulus does not name is idle:\nit holds its rest value (dashed arrow).",
           size=12)
    s.text(470, 372, "Every [in] section is recorded in every trial, whichever outputs are driven.  skip = yes in a profile drops one.",
           size=12, fill=GREY)
    s.save("fig-multipatch.svg", "Multi-patch addressing",
           "The stimulus names only P1, so ao0 is driven and ao1 holds its rest value, while both analog inputs ai0 and ai1 are recorded.")


def milestones():
    s = Svg(940, 330)
    s.text(470, 24, "Milestones and where the project stands", size=17, weight="bold")
    rows = [
        ("M0", "skeleton, Makefile,\nStimGen pin, inih", "built"),
        ("M1", "config, profiles,\nCLI, -n dry run", "built"),
        ("M2", "StimGen integration,\nunits, clip check", "built"),
        ("M3", "trial engine,\nloopback simulator", "built"),
        ("M3b", "NEURON backend\n(virtual cells)", "plan"),
        ("M4", "HDF5 + run folder,\nlog, checksums", "plan"),
        ("M5", "simulator test\nsuite", "plan"),
    ]
    x = 15
    for name, desc, k in rows:
        s.box(x, 55, 124, 100, desc, k, size=12, title=name)
        x += 130
    s.text(470, 185, "software only: everything above runs on a laptop with no hardware", size=13, fill=GREY)
    hw = [("S1", "hardware spike\n(throwaway C)", "wait"), ("M6", "DAQmx backend", "wait"),
          ("M7", "bring-up on\nthe rig", "wait"), ("M8", "docs, v0.1", "plan")]
    x = 150
    for name, desc, k in hw:
        s.box(x, 215, 150, 80, desc, k, size=12, title=name)
        x += 170
    s.text(470, 315, "hardware: waits for the rig host (USB-6343 is not supported by NI-DAQmx on Linux)", size=13, fill=GREY)
    s.save("fig-milestones.svg", "Milestones",
           "M0 and M1 are built. M2 to M5 are software-only milestones to do. S1, M6 and M7 need the hardware host to be decided; M8 is documentation.")


def stimgen_flow():
    s = Svg(940, 400)
    s.text(470, 24, "How a protocol becomes trials (StimGen 2) and then a run (LCG2): built up to the backend", size=17, weight="bold")
    s.box(20, 60, 190, 80, "sweep amp = -300..300pA\nrepeat 3, order shuffled\nperiod 5s", "blue", size=12, title="protocol text")
    s.box(250, 60, 190, 80, "ordered list of trials,\neach with condition,\nseed, timing", "built", size=12, title="sg_api_expand")
    s.box(480, 60, 200, 80, "one trial at a time:\nsamples of every channel\nin pA / mV / 0-1", "built", size=12, title="sg_api_render_trial")
    s.box(720, 60, 200, 80, "units checked against [out]\nwanted and written volts\nchecked against limits", "built", size=12, title="LCG2 checks (M2)")
    for a, b in ((210, 250), (440, 480), (680, 720)):
        s.arrow(a, 100, b, 100)
    s.text(470, 180, "Before the run, EVERY trial is rendered and checked. If any sample would clip, nothing is played and the error names trial, channel and sample.",
           size=13)
    s.box(20, 215, 210, 80, "trial 1, 2, 3 ... N\none after the other,\nrendered again when needed", "built", size=12, title="engine (M3)")
    s.box(270, 215, 210, 80, "write outputs, read inputs,\nrest values in between", "built", size=12, title="backend (loopback)")
    s.box(520, 215, 190, 80, "block per trial,\nflushed after each", "plan", size=12, title="HDF5 writer (M4)")
    s.box(750, 215, 170, 80, "config, command,\nstimulus, trials,\nlog, SHA-256", "plan", size=12, title="hidden folder")
    for a, b in ((230, 270), (480, 520), (710, 750)):
        s.arrow(a, 255, b, 255)
    s.text(470, 335, "Rendering is deterministic: the same text, rate and seed give the same samples on a given platform (StimGen reproducibility level B).",
           size=12, fill=GREY)
    s.text(470, 357, "LCG2 only calls functions named sg_api_* and never edits StimGen; the version is pinned in STIMGEN_PIN.",
           size=12, fill=GREY)
    s.save("fig-stimgen-flow.svg", "From protocol to run",
           "A protocol is expanded into trials, each trial is rendered, LCG2 checks units and limits for all trials, then the engine plays them one by one through a backend and the HDF5 writer and archive record them.")


if __name__ == "__main__":
    overview(); signal_chain(); layers(); timeline(); multipatch(); milestones(); stimgen_flow()
    print("figures written to", HERE)
