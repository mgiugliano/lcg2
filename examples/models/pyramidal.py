"""A Python cell for LCG2: a soma with Hodgkin-Huxley channels and one apical dendrite.

LCG2 calls build(...) once per [cell] that says  model = pyramidal.py.  The keys of the
[cell] section that LCG2 does not know itself arrive here as keyword arguments, with the
types converted (numbers, yes/no).  Return the cell: any object whose attributes are the
sections.  The electrode sits at soma(0.5) unless the [cell] says  electrode = apical(0.5).
"""
from neuron import h


class Pyramidal:
    def __init__(self, soma_diam=25.0, apical_length=300.0, gnabar=0.12):
        self.soma = h.Section(name="soma", cell=self)
        self.soma.L = self.soma.diam = soma_diam
        self.soma.insert("hh")
        self.soma.gnabar_hh = gnabar
        self.apical = h.Section(name="apical", cell=self)
        self.apical.L, self.apical.diam, self.apical.nseg = apical_length, 2.0, 9
        self.apical.insert("pas")
        self.apical.g_pas = 0.0001
        self.apical.e_pas = -65
        self.apical.connect(self.soma(1))

    def __str__(self):
        return "pyramidal"


def build(soma_diam=25.0, apical_length=300.0, gnabar=0.12):
    return Pyramidal(soma_diam, apical_length, gnabar)
