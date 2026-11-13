#!/bin/sh
# judge/verify.sh - Problem 4 harness. See verify.py --help.
#   ./judge/verify.sh --config verify.yaml [--runs 10] [--no-perf] [--verbose]
exec python3 "$(dirname "$0")/verify.py" "$@"
