: a small custom leak, to show that LCG2 compiles .mod files by itself
NEURON {
    SUFFIX slowleak
    NONSPECIFIC_CURRENT i
    RANGE g, e
}
PARAMETER { g = 0.00005 (S/cm2)  e = -70 (mV) }
ASSIGNED { v (mV)  i (mA/cm2) }
BREAKPOINT { i = g*(v - e) }
