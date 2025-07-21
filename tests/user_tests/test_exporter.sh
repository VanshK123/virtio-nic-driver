#!/bin/bash
set -e

PORT=$(shuf -i 9000-9999 -n 1)
telemetry-exporter "$PORT" &
PID=$!
sleep 1

curl -s "http://localhost:$PORT/metrics" | grep -q virtio_nic_tx
curl -s "http://localhost:$PORT/metrics" | grep -q virtio_nic_rx

kill $PID
