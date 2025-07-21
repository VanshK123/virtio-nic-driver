#!/bin/bash
# Simulate AZ failover for virtio-nic driver

set -e

PRIMARY_ID="$1"
SECONDARY_ID="$2"
DURATION=${3:-30}

if [[ -z "$PRIMARY_ID" || -z "$SECONDARY_ID" ]]; then
    echo "Usage: $0 <primary-instance-id> <secondary-instance-id> [duration]" >&2
    exit 1
fi

get_ip() {
    aws ec2 describe-instances --instance-ids "$1" --query 'Reservations[0].Instances[0].PublicIpAddress' --output text
}

PRIMARY_IP=$(get_ip "$PRIMARY_ID")
SECONDARY_IP=$(get_ip "$SECONDARY_ID")

ssh -o StrictHostKeyChecking=no ec2-user@${PRIMARY_IP} "nohup iperf3 -s > /tmp/iperf_server.log 2>&1 &"
ssh -o StrictHostKeyChecking=no ec2-user@${SECONDARY_IP} "nohup iperf3 -s > /tmp/iperf_server.log 2>&1 &"

echo "Measuring baseline performance against primary"
iperf3 -c ${PRIMARY_IP} -t ${DURATION} > baseline.txt

echo "Stopping primary instance to simulate failure"
aws ec2 stop-instances --instance-ids ${PRIMARY_ID}

START=$(date +%s)
until iperf3 -c ${SECONDARY_IP} -t ${DURATION} > failover.txt; do
    sleep 5
done
END=$(date +%s)

LOSS=$(grep -o "[0-9.]*%" failover.txt | tail -n1)
RECOVERY=$((END-START))

echo "Packet loss during failover: ${LOSS}" | tee results.txt
echo "Recovery time: ${RECOVERY}s" | tee -a results.txt
