#!/bin/bash
# Regenerates the sample outputs in this folder from the program itself, so that
# the manual never quotes text the program no longer prints. Run it from the top
# folder of the project, after `make`:
#
#     bash docs/outputs/regenerate.sh
#
# The drawn seed in the plans changes at every run, so the files are not expected
# to be byte-identical from one run to the next.
cd "$(dirname "$0")/../.." || exit 1
out=docs/outputs
run() { echo "\$ ./lcg2 $*"; ./lcg2 "$@" 2>&1; echo "(exit status $?)"; echo; }

./lcg2 -h                                                       > $out/help.txt
./lcg2 -c examples/single_pipette.ini -n examples/step.sg       > $out/plan_cc.txt
./lcg2 -c examples/lcg2.ini -n -m vc -P swap examples/step_vc.sg > $out/plan_vc_swap.txt
./lcg2 -c examples/lcg2.ini -n -v -P single examples/step.sg    > $out/plan_verbose.txt
{
  run -c examples/lcg2.ini -n -T TTL examples/ramp.sg
  run -c examples/lcg2.ini -n -r 300kHz examples/ramp.sg
  run -c examples/lcg2.ini -n -P nope examples/step.sg
  run -c examples/lcg2.ini -n -r 20kHZ examples/step.sg
  run -c examples/lcg2.ini -n -t 30
  run -c examples/lcg2.ini -n -m ic examples/step.sg
  run -n -c nosuch.ini examples/step.sg
  run -c examples/low_limit.ini -n examples/step.sg
  run -c examples/single_pipette.ini -n -m vc examples/step.sg
  run -c examples/lcg2.ini -n -P single examples/pair.sg
  run -c examples/lcg2.ini examples/step.sg
} > $out/errors.txt 2>&1
echo "wrote $out/*.txt"
