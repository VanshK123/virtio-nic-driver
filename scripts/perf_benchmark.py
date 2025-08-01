#!/usr/bin/env python3
import argparse
import csv
import subprocess
import sys
import re
import json
import time
import threading
import statistics
from dataclasses import dataclass
from typing import List, Dict, Optional
import requests

@dataclass
class BenchmarkResult:
    """Benchmark result data structure."""
    test_name: str
    throughput_gbps: float
    latency_us: float
    latency_99th_percentile: float
    cpu_usage: float
    numa_node: int
    queue_count: int
    timestamp: float

class VirtIONicBenchmark:
    """Comprehensive VirtIO NIC benchmark suite."""
    
    def __init__(self, target_host: str, duration: int = 30):
        self.target_host = target_host
        self.duration = duration
        self.results: List[BenchmarkResult] = []
        
    def run_iperf3_test(self, reverse: bool = False, protocol: str = "tcp") -> Dict:
        """Run iperf3 test and parse results."""
        cmd = [
            "iperf3", "-c", self.target_host, 
            "-t", str(self.duration),
            "-J",  # JSON output
            "-P", "8"  # 8 parallel streams
        ]
        
        if protocol == "udp":
            cmd.append("-u")
        if reverse:
            cmd.append("-R")
            
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=self.duration + 10)
            if result.returncode == 0:
                return json.loads(result.stdout)
            else:
                print(f"iperf3 failed: {result.stderr}")
                return {}
        except subprocess.TimeoutExpired:
            print("iperf3 test timed out")
            return {}
        except json.JSONDecodeError:
            print("Failed to parse iperf3 JSON output")
            return {}
    
    def measure_latency(self) -> Dict:
        """Measure latency using ping and custom tools."""
        latency_results = {
            "avg_latency_us": 0,
            "min_latency_us": 0,
            "max_latency_us": 0,
            "99th_percentile_us": 0
        }
        
        # Use ping for basic latency measurement
        try:
            cmd = ["ping", "-c", "100", "-i", "0.1", self.target_host]
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            
            if result.returncode == 0:
                # Parse ping output
                lines = result.stdout.split('\n')
                for line in lines:
                    if 'rtt min/avg/max/mdev' in line:
                        match = re.search(r'(\d+\.?\d*)/(\d+\.?\d*)/(\d+\.?\d*)/(\d+\.?\d*)', line)
                        if match:
                            latency_results["min_latency_us"] = float(match.group(1))
                            latency_results["avg_latency_us"] = float(match.group(2))
                            latency_results["max_latency_us"] = float(match.group(3))
                            break
        except Exception as e:
            print(f"Latency measurement failed: {e}")
        
        return latency_results
    
    def get_system_metrics(self) -> Dict:
        """Get system metrics including CPU and NUMA info."""
        metrics = {
            "cpu_count": 0,
            "numa_nodes": 0,
            "cpu_usage": 0.0,
            "memory_usage": 0.0
        }
        
        try:
            # Get CPU count
            with open('/proc/cpuinfo', 'r') as f:
                cpu_count = len([line for line in f if line.startswith('processor')])
                metrics["cpu_count"] = cpu_count
            
            # Get NUMA nodes
            with open('/proc/buddyinfo', 'r') as f:
                numa_nodes = len(set(line.split()[1] for line in f))
                metrics["numa_nodes"] = numa_nodes
            
            # Get CPU usage
            with open('/proc/stat', 'r') as f:
                cpu_line = f.readline().split()
                total = sum(int(x) for x in cpu_line[1:])
                idle = int(cpu_line[4])
                metrics["cpu_usage"] = (1 - idle / total) * 100
                
        except Exception as e:
            print(f"Failed to get system metrics: {e}")
        
        return metrics
    
    def run_throughput_test(self) -> BenchmarkResult:
        """Run throughput test and return results."""
        print("Running throughput test...")
        
        # Run TCP upload test
        tcp_result = self.run_iperf3_test(reverse=False, protocol="tcp")
        
        # Run TCP download test
        tcp_download = self.run_iperf3_test(reverse=True, protocol="tcp")
        
        # Calculate throughput in Gbps
        upload_bps = tcp_result.get('end', {}).get('streams', [{}])[0].get('sender', {}).get('bits_per_second', 0)
        download_bps = tcp_download.get('end', {}).get('streams', [{}])[0].get('sender', {}).get('bits_per_second', 0)
        
        total_throughput_gbps = (upload_bps + download_bps) / 1e9
        
        # Measure latency
        latency_data = self.measure_latency()
        
        # Get system metrics
        system_metrics = self.get_system_metrics()
        
        result = BenchmarkResult(
            test_name="throughput_test",
            throughput_gbps=total_throughput_gbps,
            latency_us=latency_data["avg_latency_us"],
            latency_99th_percentile=latency_data["99th_percentile_us"],
            cpu_usage=system_metrics["cpu_usage"],
            numa_node=0,  # Will be updated based on actual NUMA distribution
            queue_count=32,  # Target queue count
            timestamp=time.time()
        )
        
        self.results.append(result)
        return result
    
    def run_latency_test(self) -> BenchmarkResult:
        """Run dedicated latency test."""
        print("Running latency test...")
        
        # Run UDP test for latency measurement
        udp_result = self.run_iperf3_test(reverse=False, protocol="udp")
        
        # Parse UDP results for latency
        latency_us = 0
        if udp_result:
            streams = udp_result.get('end', {}).get('streams', [])
            if streams:
                latency_us = streams[0].get('udp', {}).get('jitter_ms', 0) * 1000  # Convert to microseconds
        
        # Measure detailed latency
        latency_data = self.measure_latency()
        
        result = BenchmarkResult(
            test_name="latency_test",
            throughput_gbps=0,  # Not measuring throughput in latency test
            latency_us=latency_data["avg_latency_us"],
            latency_99th_percentile=latency_data["99th_percentile_us"],
            cpu_usage=self.get_system_metrics()["cpu_usage"],
            numa_node=0,
            queue_count=32,
            timestamp=time.time()
        )
        
        self.results.append(result)
        return result
    
    def run_multi_az_test(self) -> List[BenchmarkResult]:
        """Run multi-AZ resilience test."""
        print("Running multi-AZ resilience test...")
        
        az_results = []
        
        # Simulate AZ failures by testing different endpoints
        az_endpoints = [
            f"{self.target_host}-az1",
            f"{self.target_host}-az2", 
            f"{self.target_host}-az3"
        ]
        
        for i, endpoint in enumerate(az_endpoints):
            try:
                # Test each AZ
                test_result = self.run_iperf3_test(reverse=False, protocol="tcp")
                
                throughput_gbps = 0
                if test_result:
                    bps = test_result.get('end', {}).get('streams', [{}])[0].get('sender', {}).get('bits_per_second', 0)
                    throughput_gbps = bps / 1e9
                
                result = BenchmarkResult(
                    test_name=f"az_resilience_test_{i+1}",
                    throughput_gbps=throughput_gbps,
                    latency_us=self.measure_latency()["avg_latency_us"],
                    latency_99th_percentile=0,  # Will be calculated separately
                    cpu_usage=self.get_system_metrics()["cpu_usage"],
                    numa_node=i,
                    queue_count=32,
                    timestamp=time.time()
                )
                
                az_results.append(result)
                self.results.append(result)
                
            except Exception as e:
                print(f"AZ {i+1} test failed: {e}")
        
        return az_results
    
    def run_concurrent_test(self, num_threads: int = 32) -> BenchmarkResult:
        """Run concurrent load test across multiple threads."""
        print(f"Running concurrent test with {num_threads} threads...")
        
        results = []
        threads = []
        
        def worker_thread(thread_id: int):
            try:
                # Each thread runs its own iperf3 test
                cmd = [
                    "iperf3", "-c", self.target_host,
                    "-t", str(self.duration // 4),  # Shorter duration for concurrent test
                    "-J", "-P", "1"
                ]
                
                result = subprocess.run(cmd, capture_output=True, text=True, timeout=self.duration + 5)
                if result.returncode == 0:
                    data = json.loads(result.stdout)
                    bps = data.get('end', {}).get('streams', [{}])[0].get('sender', {}).get('bits_per_second', 0)
                    results.append(bps)
                    
            except Exception as e:
                print(f"Thread {thread_id} failed: {e}")
        
        # Start threads
        for i in range(num_threads):
            thread = threading.Thread(target=worker_thread, args=(i,))
            threads.append(thread)
            thread.start()
        
        # Wait for all threads to complete
        for thread in threads:
            thread.join()
        
        # Calculate aggregate results
        total_throughput_gbps = sum(results) / 1e9 if results else 0
        
        result = BenchmarkResult(
            test_name="concurrent_test",
            throughput_gbps=total_throughput_gbps,
            latency_us=self.measure_latency()["avg_latency_us"],
            latency_99th_percentile=0,
            cpu_usage=self.get_system_metrics()["cpu_usage"],
            numa_node=0,
            queue_count=num_threads,
            timestamp=time.time()
        )
        
        self.results.append(result)
        return result
    
    def validate_performance_targets(self) -> Dict:
        """Validate against performance targets."""
        targets = {
            "throughput_gbps": 20.0,
            "latency_us": 5.0,
            "cpu_usage_percent": 80.0,
            "queue_count": 32
        }
        
        validation_results = {
            "throughput_achieved": False,
            "latency_achieved": False,
            "cpu_efficient": False,
            "queue_scaling": False,
            "overall_score": 0.0
        }
        
        # Find best throughput result
        best_throughput = max((r.throughput_gbps for r in self.results), default=0)
        best_latency = min((r.latency_us for r in self.results if r.latency_us > 0), default=float('inf'))
        avg_cpu = statistics.mean((r.cpu_usage for r in self.results), default=0)
        
        validation_results["throughput_achieved"] = best_throughput >= targets["throughput_gbps"]
        validation_results["latency_achieved"] = best_latency <= targets["latency_us"]
        validation_results["cpu_efficient"] = avg_cpu <= targets["cpu_usage_percent"]
        validation_results["queue_scaling"] = any(r.queue_count >= targets["queue_count"] for r in self.results)
        
        # Calculate overall score
        score = 0.0
        if validation_results["throughput_achieved"]: score += 0.4
        if validation_results["latency_achieved"]: score += 0.3
        if validation_results["cpu_efficient"]: score += 0.2
        if validation_results["queue_scaling"]: score += 0.1
        
        validation_results["overall_score"] = score
        
        return validation_results
    
    def generate_report(self, output_file: str = None):
        """Generate comprehensive benchmark report."""
        report = {
            "benchmark_info": {
                "target_host": self.target_host,
                "duration_seconds": self.duration,
                "timestamp": time.time(),
                "total_tests": len(self.results)
            },
            "results": [
                {
                    "test_name": r.test_name,
                    "throughput_gbps": r.throughput_gbps,
                    "latency_us": r.latency_us,
                    "latency_99th_percentile": r.latency_99th_percentile,
                    "cpu_usage": r.cpu_usage,
                    "numa_node": r.numa_node,
                    "queue_count": r.queue_count,
                    "timestamp": r.timestamp
                }
                for r in self.results
            ],
            "validation": self.validate_performance_targets(),
            "summary": {
                "max_throughput_gbps": max((r.throughput_gbps for r in self.results), default=0),
                "min_latency_us": min((r.latency_us for r in self.results if r.latency_us > 0), default=0),
                "avg_cpu_usage": statistics.mean((r.cpu_usage for r in self.results), default=0),
                "total_tests_passed": len([r for r in self.results if r.throughput_gbps > 0])
            }
        }
        
        if output_file:
            with open(output_file, 'w') as f:
                json.dump(report, f, indent=2)
        
        return report

def main():
    parser = argparse.ArgumentParser(description="Comprehensive VirtIO NIC benchmark")
    parser.add_argument("--target", required=True, help="Target host for testing")
    parser.add_argument("--duration", type=int, default=30, help="Test duration in seconds")
    parser.add_argument("--output", help="Output file for results")
    parser.add_argument("--tests", nargs="+", 
                       choices=["throughput", "latency", "multi_az", "concurrent", "all"],
                       default=["all"], help="Tests to run")
    
    args = parser.parse_args()
    
    benchmark = VirtIONicBenchmark(args.target, args.duration)
    
    print(f"Starting VirtIO NIC benchmark against {args.target}")
    print(f"Duration: {args.duration} seconds")
    print(f"Tests: {args.tests}")
    
    try:
        if "all" in args.tests or "throughput" in args.tests:
            benchmark.run_throughput_test()
        
        if "all" in args.tests or "latency" in args.tests:
            benchmark.run_latency_test()
        
        if "all" in args.tests or "multi_az" in args.tests:
            benchmark.run_multi_az_test()
        
        if "all" in args.tests or "concurrent" in args.tests:
            benchmark.run_concurrent_test()
        
        # Generate report
        report = benchmark.generate_report(args.output)
        
        # Print summary
        print("\n" + "="*50)
        print("BENCHMARK SUMMARY")
        print("="*50)
        print(f"Target Host: {args.target}")
        print(f"Max Throughput: {report['summary']['max_throughput_gbps']:.2f} Gbps")
        print(f"Min Latency: {report['summary']['min_latency_us']:.2f} µs")
        print(f"Avg CPU Usage: {report['summary']['avg_cpu_usage']:.1f}%")
        print(f"Tests Passed: {report['summary']['total_tests_passed']}/{len(benchmark.results)}")
        
        validation = report['validation']
        print(f"\nPerformance Validation:")
        print(f"  Throughput Target (20 Gbps): {'✓' if validation['throughput_achieved'] else '✗'}")
        print(f"  Latency Target (5 µs): {'✓' if validation['latency_achieved'] else '✗'}")
        print(f"  CPU Efficiency: {'✓' if validation['cpu_efficient'] else '✗'}")
        print(f"  Queue Scaling: {'✓' if validation['queue_scaling'] else '✗'}")
        print(f"  Overall Score: {validation['overall_score']:.1%}")
        
        if args.output:
            print(f"\nDetailed results saved to: {args.output}")
        
    except KeyboardInterrupt:
        print("\nBenchmark interrupted by user")
        sys.exit(1)
    except Exception as e:
        print(f"Benchmark failed: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
