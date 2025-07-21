#!/usr/bin/env python3
"""Simple performance benchmark wrapper for iperf3."""
import argparse
import csv
import subprocess
import sys
import re


def run_iperf(target: str, duration: int, reverse: bool = False) -> str:
    cmd = ["iperf3", "-c", target, "-u", "-t", str(duration)]
    if reverse:
        cmd.append("-R")
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result.stdout


def parse_stats(output: str):
    bandwidth = ""
    latency = ""
    for line in output.splitlines():
        m = re.search(r"(\d+\.\d+\s+[KMG]?bits/sec).*?(\d+\.\d+)\s+ms", line)
        if m:
            bandwidth = m.group(1)
            latency = m.group(2)
    return bandwidth, latency


def main():
    parser = argparse.ArgumentParser(description="Run iperf3 benchmark")
    parser.add_argument("--target", required=True, help="Target IP address")
    parser.add_argument("--duration", type=int, default=10, help="Test duration")
    args = parser.parse_args()

    tx_out = run_iperf(args.target, args.duration)
    rx_out = run_iperf(args.target, args.duration, reverse=True)

    tx_bw, tx_lat = parse_stats(tx_out)
    rx_bw, rx_lat = parse_stats(rx_out)

    writer = csv.writer(sys.stdout)
    writer.writerow(["direction", "bandwidth", "latency_ms"])
    writer.writerow(["tx", tx_bw, tx_lat])
    writer.writerow(["rx", rx_bw, rx_lat])


if __name__ == "__main__":
    main()
