"""A Hodgkin-Huxley soma with a hyperpolarisation-activated current (Ih), from channels/ih.mod.

[cell P1]
model      = soma_ih.py
mechanisms = channels          ; the folder with ih.mod: compiled automatically the first time
gh         = 0.0005            ; parameter of this model, S/cm2
"""
from neuron import h


class SomaIh:
    def __init__(self, gh=0.0005):
        self.soma = h.Section(name="soma", cell=self)
        self.soma.L = self.soma.diam = 20
        self.soma.insert("hh")
        self.soma.insert("ih")            # the channel from channels/ih.mod
        for seg in self.soma:
            seg.ih.gbar = gh

    def __str__(self):
        return "soma_ih"


def build(gh=0.0005):
    return SomaIh(gh)
