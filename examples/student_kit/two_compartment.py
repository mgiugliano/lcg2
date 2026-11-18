"""Your first cell, from scratch: a soma and one dendrite, with every number a parameter.

This is the file tutorial 6 builds up step by step.  LCG2 calls build(...) with the keys of the [cell] section
that it does not use itself, so  dend_length = 400  in the configuration reaches this function.
"""
from neuron import h


def build(soma_diam=20.0, dend_length=200.0, dend_diam=1.0, gna=0.12, gk=0.036, g_leak=0.0003):
    cell = type("TwoCompartment", (), {"__str__": lambda self: "two_compartment"})()
    cell.soma = h.Section(name="soma", cell=cell)             # a section is a cylinder of membrane
    cell.soma.L = cell.soma.diam = soma_diam                  # micrometres
    cell.soma.insert("hh")                                    # the Hodgkin-Huxley channels
    for seg in cell.soma:
        seg.hh.gnabar, seg.hh.gkbar, seg.hh.gl = gna, gk, g_leak
    cell.dend = h.Section(name="dend", cell=cell)
    cell.dend.L, cell.dend.diam, cell.dend.nseg = dend_length, dend_diam, 9
    cell.dend.insert("pas")                                   # a passive membrane: just a leak
    for seg in cell.dend:
        seg.pas.g, seg.pas.e = 0.0001, -65
    cell.dend.connect(cell.soma(1))                           # the dendrite starts at the end of the soma
    return cell
