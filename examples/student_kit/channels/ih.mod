: A hyperpolarisation-activated cation current (Ih, "HCN"), to show that LCG2 compiles your own channels.
: It opens when the cell is hyperpolarised and gives the "sag" seen in many neurons.
UNITS {
    (mA) = (milliamp)
    (mV) = (millivolt)
    (S)  = (siemens)
}
NEURON {
    SUFFIX ih
    NONSPECIFIC_CURRENT i
    RANGE gbar, e
}
PARAMETER {
    gbar = 0.0005 (S/cm2)
    e    = -30    (mV)
}
STATE { m }
ASSIGNED { v (mV)  i (mA/cm2)  minf  tau (ms) }
INITIAL { rates(v)  m = minf }
BREAKPOINT {
    SOLVE states METHOD cnexp
    i = gbar * m * (v - e)
}
DERIVATIVE states {
    rates(v)
    m' = (minf - m) / tau
}
PROCEDURE rates(v (mV)) {
    minf = 1 / (1 + exp((v + 72) / 6))
    tau  = 200 / (exp((v + 70) / 20) + exp(-(v + 70) / 20))
}
