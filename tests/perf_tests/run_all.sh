#!/bin/bash
# Run iperf3 performance tests
set -e

SERVER=${1:-$IPERF_SERVER}
CLIENT=${2:-$IPERF_CLIENT}
DURATION=${DURATION:-10}
RESULT_DIR=${RESULT_DIR:-results}

if [[ -z "$SERVER" || -z "$CLIENT" ]]; then
    echo "Usage: $0 <server-host> <client-host>" >&2
    exit 1
fi

mkdir -p "$RESULT_DIR"
TS=$(date +%Y%m%d%H%M%S)

ssh "$SERVER" "nohup iperf3 -s > /tmp/iperf_server.log 2>&1 &"

iperf3 -c "$SERVER" -t "$DURATION" > "$RESULT_DIR/uplink_$TS.txt"
iperf3 -c "$SERVER" -R -t "$DURATION" > "$RESULT_DIR/downlink_$TS.txt"

ssh "$SERVER" "pkill iperf3"

echo "Results stored in $RESULT_DIR"
