"""Initialisation code: runs after the cells are built and initialised (here: start the soma at -70 mV)."""
def initialize(cell):
    for seg in cell.soma:
        seg.v = -70
