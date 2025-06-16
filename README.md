# virtio-nic-driver

**Next‑Gen VirtIO Paravirtualized NIC Driver**  
Multi‑AZ optimized NIC driver delivering ≥20 Gbps throughput at sub‑5 µs latency with full telemetry and resiliency.

---

## Table of Contents

1. [Overview](#overview)  
2. [Key Features](#key-features)  
3. [Directory Layout](#directory-layout)  
4. [Prerequisites](#prerequisites)  
5. [Build & Installation](#build--installation)  
   - [Kernel Module](#kernel-module)  
   - [User‑Space Toolkit](#user-space-toolkit)  
6. [Usage](#usage)  
7. [Testing & Benchmarking](#testing--benchmarking)  
8. [Documentation](#documentation)  

---

## Overview

The **virtio-nic-driver** project implements a high-performance VirtIO paravirtualized NIC front‑end driver designed for cloud environments.  
As a solo‑engineer turnkey solution, this driver supports:
- **20 Gbps+ throughput** via zero‑copy DMA  
- **≤ 5 µs 99th‑percentile RTT** with adaptive coalescing  
- **Linear scaling** across up to 32 vCPUs  
- **Multi‑AZ resilience** with dynamic queue remapping  
- **Built‑in telemetry** for per‑flow QoS and error tracking  

---

## Key Features

- **Multi-Queue TX/RX** with scheduler to balance load  
- **MSI‑X Support** and interrupt coalescing  
- **Zero-Copy DMA** buffer management for minimal overhead  
- **Dynamic Fail‑Over**: AZ‑aware queue remapping  
- **Prometheus‑Compatible** metrics exporter  
- **Automated AWS Multi‑AZ Harness** for regression and failure injection  

---

## Directory Layout

```bash
virtio-nic-driver/
├── kernel/           # In‑kernel VirtIO front‑end driver
├── user/             # CLI loader, QoS agent, telemetry exporter
├── scripts/          # AWS provisioning & test harness
├── tests/            # Unit, integration, performance tests
├── docs/             # Whitepaper, API spec, benchmarks, runbooks
└── .gitignore        # Build & artifact exclusions
```

---

## Prerequisites

- **Host OS:** Linux distribution (kernel ≥5.x) with KVM/QEMU  
- **Toolchain:** GCC cross‑compiler, `make`, `cmake`  
- **Libraries:** `libvirt`, `libpci`, Prometheus C client  
- **AWS CLI:** Configured credentials for multi‑AZ provisioning  

---

## Build & Installation

### Kernel Module

1. Clone kernel tree and enable config:
   ```bash
   git clone https://your.git.host/virtio-nic-driver.git
   cd virtio-nic-driver
   cp kernel/Kconfig /path/to/linux/drivers/net/virtio/
   echo 'source "drivers/net/virtio/Kconfig"' >> /path/to/linux/Drivers/net/Kconfig
   ```
2. Configure kernel:
   ```bash
   cd /path/to/linux
   make menuconfig  # Enable "VirtIO Paravirtualized NIC front-end driver"
   ```
3. Build & install:
   ```bash
   make -j$(nproc)
   sudo make modules_install
   sudo depmod -a
   ```
4. Load driver:
   ```bash
   sudo modprobe virtio_nic
   ```

### User‑Space Toolkit

1. Build:
   ```bash
   mkdir -p build && cd build
   cmake ..
   make -j$(nproc)
   ```
2. Install:
   ```bash
   sudo make install
   ```
3. Components:
   - `virtio-nic-loader` — load/unload kernel module  
   - `qos-agent` — dynamic QoS policy enforcement  
   - `telemetry-exporter` — HTTP /metrics for Prometheus  

---

## Usage

```bash
# Load driver and set up QoS policy
sudo virtio-nic-loader load
sudo qos-agent --config /etc/virtio-nic/qos.yaml

# Start Prometheus exporter
telemetry-exporter --port 9100
```

Monitor metrics in Grafana by adding Prometheus job:
```yaml
- job_name: 'virtio_nic'
  static_configs:
    - targets: ['<vm-ip>:9100']
```

---

## Testing & Benchmarking

1. **AWS Multi-AZ Setup**:
   ```bash
   ./scripts/aws_multi_az_setup.sh --regions us-east-1,us-west-2
   ```
2. **Failover & Perf**:
   ```bash
   ./scripts/failover_test.sh
   python3 scripts/perf_benchmark.py
   ```
3. **Regression Suite**:
   ```bash
   cd tests
   ./run_all.sh
   ```

---

## Documentation

- **Whitepaper** (`docs/whitepaper.md`) — Architecture & trade‑offs  
- **API Spec** (`docs/api_spec.md`) — Kernel/User interfaces  
- **Benchmark Report** (`docs/benchmark_report.md`) — Throughput & latency graphs  
- **Integration Guide** (`docs/integration_guide.md`) — Step‑by‑step deployment  

---
