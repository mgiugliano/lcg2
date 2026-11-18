#!/bin/bash
# Builds docs/pdf/LCG2-manual.pdf: the README (with the installation guide), the tutorials and the NEURON tutorials.
#
#     make pdf        (or: bash docs/pdf/build.sh)
#
# Needs pandoc, xelatex and rsvg-convert (the figures are SVG).  `make doctor` says what is missing.
set -e
cd "$(dirname "$0")/../.."
for t in pandoc xelatex rsvg-convert; do
  command -v $t >/dev/null || { echo "pdf: $t is not installed (make doctor says how to install it)"; exit 1; }
done
version=$(sed -n 's/^#define LCG2_VERSION "\(.*\)"/\1/p' src/lcg2.h)
out=docs/pdf/LCG2-manual.pdf
work=$(mktemp -d)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$work"' EXIT

# one page title is the chapter title: the README's "# LCG2" becomes "Overview and installation"
sed '1s/^# LCG2$/# Overview and installation/' README.md > "$work/01-readme.md"
sed '1s/^# .*/# Tutorials: the simulator rig/' docs/03-tutorials.md > "$work/02-tutorials.md"
sed '1s/^# .*/# Tutorials with virtual cells/' docs/16-neuron-tutorials.md > "$work/03-neuron-tutorials.md"
sed '1s/^# .*/# The student kit: exercises/' examples/student_kit/README.md > "$work/04-student-kit.md"

cat > "$work/meta.yaml" <<YAML
---
title: "LCG2: manual and tutorials"
subtitle: "Version $version"
author: "Michele Giugliano"
date: "$(date +%Y-%m-%d)"
documentclass: report
classoption: [11pt, a4paper, oneside]
geometry: "margin=2.4cm"
mainfont: "Helvetica Neue"
monofont: "Menlo"
colorlinks: true
linkcolor: blue
urlcolor: blue
toc: true
toc-depth: 2
---
YAML

pandoc "$work/meta.yaml" "$work"/0*.md \
  --from markdown+pipe_tables+backtick_code_blocks+fenced_code_attributes+implicit_figures \
  --pdf-engine=xelatex \
  --top-level-division=chapter \
  --resource-path=.:docs:examples/student_kit \
  --lua-filter=docs/pdf/filter.lua \
  --include-in-header=docs/pdf/header.tex \
  --syntax-highlighting=tango \
  -o "$out" 2> "$work/pandoc.log" || { cat "$work/pandoc.log"; exit 1; }
grep -i "missing character\|could not\|error" "$work/pandoc.log" | head -10 || true
echo "pdf: wrote $out ($(du -h "$out" | cut -f1))"
