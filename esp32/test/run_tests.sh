#!/bin/bash
# Car Sentry — host unit tests (test/run_tests.sh)
# Compiles test_logic.cpp with plain g++ (no ESP32 / PlatformIO needed) and
# runs it. Tests: alarm state machine, sleep/wake decisions, protocol
# parsing/building + WoL packet, LD2410 frame parser.
# Usage: ./test/run_tests.sh   (exit 0 = all pass)
set -e
cd "$(dirname "$0")/.."
g++ -std=c++17 -Wall -Wextra -I src -I include -I test \
  test/test_logic.cpp -o /tmp/car-sentry-test-logic
/tmp/car-sentry-test-logic
