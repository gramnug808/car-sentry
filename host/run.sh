#!/bin/bash
# Car Sentry host — run from ~/workspace/car-sentry/host/
#   ./run.sh --self-test   |   ./run.sh --ui   |   ./run.sh --run
cd "$(dirname "$0")"
exec python3 -m car_sentry "$@"
