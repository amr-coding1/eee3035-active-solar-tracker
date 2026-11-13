#!/usr/bin/env bash
# run_tests.sh - all tests of the analysis side (Agent M's part of the AST data link).
#
#   analysis/tests/run_tests.sh            run everything, exit 0 when all pass
#
#   1. pytest            analysis/py/test_ast_metrics.py (hand-computed fixtures, contract section 6)
#   2. MATLAB lint       no Octave-only syntax and no API keys in analysis/matlab/*.m
#   3. nightly check     ast_nightly.m in GNU Octave against the shared mock ThingSpeak
#                        (tools/mock_thingspeak.py) and the fixture server; every channel-2
#                        row is compared with the Python reference (text, rule 6.5, tolerances)
#   4. vis/report check  the five vis_*.m and ast_report_figures.m in Octave (off screen)
#
# Needs: python3 + pytest, octave (8.x), curl, xvfb-run for step 4. Servers are started on
# free local ports and stopped by the Python scripts themselves. Nothing here talks to the
# real ThingSpeak service.
set -u
cd "$(dirname "$0")"
status=0
step() { echo; echo "=== $1 ==="; }

step "1/4 pytest (Python reference)"
python3 -m pytest -q ../py || status=1

step "2/4 MATLAB syntax lint"
python3 matlab_lint.py || status=1

if command -v octave >/dev/null 2>&1; then
    step "3/4 ast_nightly.m in Octave against the reference"
    python3 nightly_check.py || status=1
    step "4/4 visualisations and report figures in Octave"
    python3 vis_check.py || status=1
else
    echo "octave not found: steps 3 and 4 NOT RUN"
    status=1
fi

echo
if [ "$status" -eq 0 ]; then echo "ALL ANALYSIS TESTS PASSED"; else echo "SOME ANALYSIS TESTS FAILED"; fi
exit "$status"
