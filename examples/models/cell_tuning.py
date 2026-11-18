"""Biophysics for tiny.swc: HH in the soma, passive dendrites.  `cell` and `h` are given to this file."""
def biophysics(cell, gnabar=0.12):
    for sec in cell.all:
        sec.Ra, sec.cm = 100, 1
        if sec is cell.soma:
            sec.insert("hh")
            sec.gnabar_hh = gnabar
        else:
            sec.insert("pas")
            sec.g_pas, sec.e_pas = 0.0001, -65
