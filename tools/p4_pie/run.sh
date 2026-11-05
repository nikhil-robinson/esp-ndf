#!/bin/bash
# Host check of the P4 vector kernels: needs `pip install unicorn "setuptools<70"` (a venv is fine: PYTHON=/path/python).
# Builds components/lib/esp32p4/{dsp,nn}_pie.S with the P4 toolchain and runs them in the instruction-level model (sim.py).
set -e
cd "$(dirname "$0")"
PY=${PYTHON:-python3}
mkdir -p /tmp/claude-1000
$PY test_dsp.py && $PY test_nn.py && $PY test_nn_layers.py
