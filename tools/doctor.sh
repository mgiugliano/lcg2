#!/bin/bash
# LCG2 environment check:  make doctor   (or: bash tools/doctor.sh)
# Says what is installed, what is missing and how to install it. Exit status 1 if something REQUIRED is missing.
cd "$(dirname "$0")/.." || exit 1
bad=0
ok()   { printf '  ok       %s\n' "$1"; }
miss() { printf '  MISSING  %s\n           %s\n' "$1" "$2"; }
opt()  { printf '  optional %s\n           %s\n' "$1" "$2"; }
case "$(uname -s)" in Darwin) os=mac;; *) os=linux;; esac
inst_cc="xcode-select --install"; inst_h5="brew install hdf5"; inst_py="brew install python@3.11"
[ $os = linux ] && { inst_cc="sudo apt install build-essential"; inst_h5="sudo apt install libhdf5-dev"; inst_py="sudo apt install python3 python3-venv python3-pip"; }

echo "LCG2 $(sed -n 's/^#define LCG2_VERSION "\(.*\)"/\1/p' src/lcg2.h): environment check ($(uname -sm))"
echo; echo "required to build and test:"
if command -v cc >/dev/null; then ok "C compiler: $(cc --version | head -1)"; else miss "a C99 compiler (cc)" "$inst_cc"; bad=1; fi
if command -v make >/dev/null; then ok "make: $(make --version | head -1)"; else miss "make" "$inst_cc"; bad=1; fi
if out=$(make -s check-hdf5 2>&1); then ok "HDF5 compiler wrapper: ${out#check-hdf5: using }"; else miss "HDF5 (h5cc)" "$inst_h5"; bad=1; fi
if out=$(make -s check-pin 2>&1); then ok "StimGen 2 sources match the pin ($(sed -n 's/^commit *= *//p' STIMGEN_PIN | cut -c1-10))"; else
  miss "StimGen 2 at the pinned commit (next to this folder, or STIMGEN_SRC=...)" "git clone https://github.com/mgiugliano/StimGen2.git ../StimGen && git -C ../StimGen checkout $(sed -n 's/^commit *= *//p' STIMGEN_PIN)"; bad=1; fi
if command -v python3 >/dev/null; then ok "python3: $(python3 --version 2>&1)"; else miss "python3 (to run the tests)" "$inst_py"; bad=1; fi

echo; echo "to read data and run the recording tests:"
py=""
for c in "$LCG2_H5PY_PYTHON" python3 python3.11 python3.12 python3.10; do
  [ -n "$c" ] && command -v "$c" >/dev/null && "$c" -I -c "import h5py" 2>/dev/null && { py=$c; break; }
done
if [ -n "$py" ]; then ok "h5py $("$py" -I -c 'import h5py; print(h5py.__version__)') (in $(command -v "$py"))"; else
  miss "a Python with h5py" "pip install h5py   (in a virtual environment: see requirements.txt)"; fi

echo; echo "for device = neuron (optional):"
npy=""
for c in "$LCG2_NEURON_PYTHON" python3 python3.11 python3.12 python3.10 /opt/homebrew/opt/python@3.11/bin/python3.11; do
  [ -n "$c" ] && command -v "$c" >/dev/null && "$c" -I -c "import neuron" 2>/dev/null && { npy=$(command -v "$c"); break; }
done
if [ -n "$npy" ]; then ok "NEURON $("$npy" -I -c 'import neuron; print(neuron.__version__)' 2>/dev/null | tail -1) (in $npy): use it as sim_python"; else
  opt "no Python with NEURON found" "pip install neuron   (then set sim_python in your configuration to that Python)"; fi
if [ -n "$npy" ]; then "$npy" -I -c "import matplotlib" 2>/dev/null && ok "matplotlib (plot_run.py, the figures)" || opt "matplotlib" "pip install matplotlib"; fi

echo; echo "for the PDF manual (optional):"
for t in pandoc xelatex rsvg-convert; do
  if command -v $t >/dev/null; then ok "$t"; else
    case $os in mac) h="brew install pandoc librsvg; brew install --cask mactex-no-gui";; *) h="sudo apt install pandoc texlive-xetex texlive-fonts-recommended librsvg2-bin";; esac
    opt "$t" "$h"; fi
done
echo
if [ $bad -eq 0 ]; then echo "Everything required is there: make && make test"; else echo "Something required is missing (see above)."; fi
exit $bad
