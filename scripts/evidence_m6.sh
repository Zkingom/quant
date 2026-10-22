#!/usr/bin/env bash
# M6 correctness and recovery regression after the optimization: the complete
# M5 evidence run (six presets, io / alloc / differential tests, process
# crashes, CLI golden, M4 soak matrix, M5/M3/M2 mutation tables) written to
# MODULE_NOTES/evidence/M6/regression/. Same options as evidence_m5.sh (RESUME=1).
# Performance evidence is kept apart, in MODULE_NOTES/evidence/M6/{baseline_*,compare_*,profile}.
cd "$(dirname "$0")/.."
EVIDENCE_DIR=MODULE_NOTES/evidence/M6/regression exec bash scripts/evidence_m5.sh
