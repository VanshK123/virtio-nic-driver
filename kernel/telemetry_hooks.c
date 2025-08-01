#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/netdevice.h>
#include <linux/sysfs.h>
#include <linux/ktime.h>
#include <linux/numa.h>
#include <linux/cpumask.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include "virtio_nic.h"

/* Global telemetry instance */
static struct virtio_nic_telemetry *global_telemetry;

/* Performance counters */
static struct perf_event *tx_event;
static struct perf_event *rx_event;
static struct perf_event *latency_event;
static struct perf_event *throughput_event;

/* Sysfs attributes */
static struct kobject *telemetry_kobj;
static struct kobj_attribute tx_attr;
static struct kobj_attribute rx_attr;
static struct kobj_attribute latency_attr;
static struct kobj_attribute throughput_attr;
static struct kobj_attribute queue_stats_attr;
static struct kobj_attribute flow_stats_attr;
static struct kobj_attribute numa_stats_attr;

/* Statistics tracking */
static atomic64_t total_tx_packets = ATOMIC64_INIT(0);
static atomic64_t total_rx_packets = ATOMIC64_INIT(0);
static atomic64_t total_tx_bytes = ATOMIC64_INIT(0);
static atomic64_t total_rx_bytes = ATOMIC64_INIT(0);
static atomic64_t total_latency_ns = ATOMIC64_INIT(0);
static atomic64_t latency_samples = ATOMIC64_INIT(0);

/* Flow tracking for per-flow metrics */
struct virtio_nic_flow_metric {
    u32 flow_id;
    u64 packets;
    u64 bytes;
    u64 latency_sum;
    u64 latency_count;
    u64 last_seen;
    struct list_head list;
    spinlock_t lock;
};

static LIST_HEAD(flow_metrics_list);
static spinlock_t flow_metrics_lock = SPIN_LOCK_UNLOCKED;

/* NUMA statistics */
struct virtio_nic_numa_stats {
    int numa_node;
    u64 rx_packets;
    u64 tx_packets;
    u64 rx_bytes;
    u64 tx_bytes;
    u64 rx_errors;
    u64 tx_errors;
};

static struct virtio_nic_numa_stats numa_stats[NR_NUMA_NODES];

/* Sysfs show functions */
static ssize_t tx_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    u64 val = atomic64_read(&total_tx_packets);
    return sprintf(buf, "%llu\n", val);
}

static ssize_t rx_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    u64 val = atomic64_read(&total_rx_packets);
    return sprintf(buf, "%llu\n", val);
}

static ssize_t latency_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    u64 samples = atomic64_read(&latency_samples);
    u64 total = atomic64_read(&total_latency_ns);
    u64 avg_latency = samples > 0 ? total / samples : 0;
    return sprintf(buf, "%llu\n", avg_latency);
}

static ssize_t throughput_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    u64 tx_bytes = atomic64_read(&total_tx_bytes);
    u64 rx_bytes = atomic64_read(&total_rx_bytes);
    u64 total_bytes = tx_bytes + rx_bytes;
    return sprintf(buf, "%llu\n", total_bytes);
}

/* Enhanced queue statistics */
static ssize_t queue_stats_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct virtio_nic_priv *priv = NULL;
    int i, len = 0;
    char *pos = buf;

    /* Find the first available priv structure */
    for (i = 0; i < 10; i++) {
        struct net_device *ndev = dev_get_by_name(&init_net, "virtio_nic");
        if (ndev) {
            priv = netdev_priv(ndev);
            dev_put(ndev);
            break;
        }
    }

    if (!priv) {
        return sprintf(buf, "No device found\n");
    }

    len += sprintf(pos + len, "Queue Statistics:\n");
    len += sprintf(pos + len, "Queue\tNUMA\tCPU\tRX_Pkts\tTX_Pkts\tRX_Bytes\tTX_Bytes\tPending\n");

    for (i = 0; i < priv->num_queues; i++) {
        struct virtio_nic_queue *q = &priv->queues[i];
        len += sprintf(pos + len, "%d\t%d\t%d\t%llu\t%llu\t%llu\t%llu\t%d\n",
                      i, q->numa_node, q->cpu_id,
                      q->rx_packets, q->tx_packets,
                      q->rx_bytes, q->tx_bytes,
                      atomic_read(&q->pending_packets));
    }

    return len;
}

/* Flow statistics for per-flow monitoring */
static ssize_t flow_stats_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct virtio_nic_flow_metric *flow;
    int len = 0;
    char *pos = buf;
    unsigned long flags;

    len += sprintf(pos + len, "Flow Statistics:\n");
    len += sprintf(pos + len, "Flow_ID\tPackets\tBytes\tAvg_Latency(ns)\tLast_Seen\n");

    spin_lock_irqsave(&flow_metrics_lock, flags);
    list_for_each_entry(flow, &flow_metrics_list, list) {
        u64 avg_latency = flow->latency_count > 0 ? 
                          flow->latency_sum / flow->latency_count : 0;
        len += sprintf(pos + len, "%u\t%llu\t%llu\t%llu\t%llu\n",
                      flow->flow_id, flow->packets, flow->bytes,
                      avg_latency, flow->last_seen);
    }
    spin_unlock_irqrestore(&flow_metrics_lock, flags);

    return len;
}

/* NUMA statistics */
static ssize_t numa_stats_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    int i, len = 0;
    char *pos = buf;

    len += sprintf(pos + len, "NUMA Statistics:\n");
    len += sprintf(pos + len, "NUMA\tRX_Pkts\tTX_Pkts\tRX_Bytes\tTX_Bytes\tErrors\n");

    for (i = 0; i < num_possible_nodes(); i++) {
        struct virtio_nic_numa_stats *stats = &numa_stats[i];
        len += sprintf(pos + len, "%d\t%llu\t%llu\t%llu\t%llu\t%llu\n",
                      i, stats->rx_packets, stats->tx_packets,
                      stats->rx_bytes, stats->tx_bytes,
                      stats->rx_errors + stats->tx_errors);
    }

    return len;
}

/* Initialize telemetry system */
void telemetry_init(struct net_device *ndev)
{
    struct perf_event_attr attr = {
        .type = PERF_TYPE_SOFTWARE,
        .config = PERF_COUNT_SW_CPU_CLOCK,
        .size = sizeof(struct perf_event_attr),
    };

    /* Initialize performance events */
    tx_event = perf_event_create_kernel_counter(&attr, -1, NULL, NULL, NULL);
    rx_event = perf_event_create_kernel_counter(&attr, -1, NULL, NULL, NULL);
    
    /* Setup latency tracking */
    attr.config = PERF_COUNT_SW_CONTEXT_SWITCHES;
    latency_event = perf_event_create_kernel_counter(&attr, -1, NULL, NULL, NULL);
    
    /* Setup throughput tracking */
    attr.config = PERF_COUNT_SW_CPU_MIGRATIONS;
    throughput_event = perf_event_create_kernel_counter(&attr, -1, NULL, NULL, NULL);

    /* Create sysfs interface */
    telemetry_kobj = kobject_create_and_add("virtio_nic_telemetry", &ndev->dev.kobj);
    if (telemetry_kobj) {
        /* Setup attributes */
        tx_attr.attr.name = "tx_packets";
        tx_attr.attr.mode = 0444;
        tx_attr.show = tx_show;
        sysfs_create_file(telemetry_kobj, &tx_attr.attr);

        rx_attr.attr.name = "rx_packets";
        rx_attr.attr.mode = 0444;
        rx_attr.show = rx_show;
        sysfs_create_file(telemetry_kobj, &rx_attr.attr);

        latency_attr.attr.name = "avg_latency_ns";
        latency_attr.attr.mode = 0444;
        latency_attr.show = latency_show;
        sysfs_create_file(telemetry_kobj, &latency_attr.attr);

        throughput_attr.attr.name = "total_bytes";
        throughput_attr.attr.mode = 0444;
        throughput_attr.show = throughput_show;
        sysfs_create_file(telemetry_kobj, &throughput_attr.attr);

        queue_stats_attr.attr.name = "queue_stats";
        queue_stats_attr.attr.mode = 0444;
        queue_stats_attr.show = queue_stats_show;
        sysfs_create_file(telemetry_kobj, &queue_stats_attr.attr);

        flow_stats_attr.attr.name = "flow_stats";
        flow_stats_attr.attr.mode = 0444;
        flow_stats_attr.show = flow_stats_show;
        sysfs_create_file(telemetry_kobj, &flow_stats_attr.attr);

        numa_stats_attr.attr.name = "numa_stats";
        numa_stats_attr.attr.mode = 0444;
        numa_stats_attr.show = numa_stats_show;
        sysfs_create_file(telemetry_kobj, &numa_stats_attr.attr);
    }

    /* Initialize NUMA statistics */
    memset(numa_stats, 0, sizeof(numa_stats));
    for (int i = 0; i < num_possible_nodes(); i++) {
        numa_stats[i].numa_node = i;
    }
}
EXPORT_SYMBOL_GPL(telemetry_init);

void telemetry_exit(void)
{
    struct virtio_nic_flow_metric *flow, *tmp;

    /* Cleanup performance events */
    if (tx_event)
        perf_event_release_kernel(tx_event);
    if (rx_event)
        perf_event_release_kernel(rx_event);
    if (latency_event)
        perf_event_release_kernel(latency_event);
    if (throughput_event)
        perf_event_release_kernel(throughput_event);

    /* Cleanup flow metrics */
    spin_lock(&flow_metrics_lock);
    list_for_each_entry_safe(flow, tmp, &flow_metrics_list, list) {
        list_del(&flow->list);
        kfree(flow);
    }
    spin_unlock(&flow_metrics_lock);

    /* Cleanup sysfs */
    if (telemetry_kobj) {
        kobject_put(telemetry_kobj);
        telemetry_kobj = NULL;
    }
}
EXPORT_SYMBOL_GPL(telemetry_exit);

void telemetry_record_tx(void)
{
    atomic64_inc(&total_tx_packets);
    if (tx_event)
        perf_event_inc(tx_event);
}
EXPORT_SYMBOL_GPL(telemetry_record_tx);

void telemetry_record_rx(void)
{
    atomic64_inc(&total_rx_packets);
    if (rx_event)
        perf_event_inc(rx_event);
}
EXPORT_SYMBOL_GPL(telemetry_record_rx);

void telemetry_record_latency(u64 latency_ns)
{
    atomic64_add(latency_ns, &total_latency_ns);
    atomic64_inc(&latency_samples);
    
    if (latency_event)
        perf_event_inc(latency_event);
}
EXPORT_SYMBOL_GPL(telemetry_record_latency);

void telemetry_update_queue_stats(struct virtio_nic_queue *q)
{
    if (!q)
        return;

    /* Update NUMA statistics */
    if (q->numa_node < num_possible_nodes()) {
        struct virtio_nic_numa_stats *stats = &numa_stats[q->numa_node];
        stats->rx_packets += q->rx_packets;
        stats->tx_packets += q->tx_packets;
        stats->rx_bytes += q->rx_bytes;
        stats->tx_bytes += q->tx_bytes;
        stats->rx_errors += q->rx_errors;
        stats->tx_errors += q->tx_errors;
    }
}
EXPORT_SYMBOL_GPL(telemetry_update_queue_stats);

void telemetry_update_flow_stats(struct virtio_nic_flow *flow)
{
    struct virtio_nic_flow_metric *metric;
    unsigned long flags;

    if (!flow)
        return;

    spin_lock_irqsave(&flow_metrics_lock, flags);
    
    /* Find existing metric or create new one */
    list_for_each_entry(metric, &flow_metrics_list, list) {
        if (metric->flow_id == flow->flow_id) {
            metric->packets += flow->packets;
            metric->bytes += flow->bytes;
            metric->last_seen = flow->last_seen;
            goto out;
        }
    }

    /* Create new metric */
    metric = kzalloc(sizeof(*metric), GFP_ATOMIC);
    if (metric) {
        metric->flow_id = flow->flow_id;
        metric->packets = flow->packets;
        metric->bytes = flow->bytes;
        metric->last_seen = flow->last_seen;
        spin_lock_init(&metric->lock);
        list_add_tail(&metric->list, &flow_metrics_list);
    }

out:
    spin_unlock_irqrestore(&flow_metrics_lock, flags);
}
EXPORT_SYMBOL_GPL(telemetry_update_flow_stats);

/* Get telemetry statistics for Prometheus export */
void telemetry_get_stats(struct virtio_nic_telemetry_stats *stats)
{
    if (!stats)
        return;

    stats->tx_packets = atomic64_read(&total_tx_packets);
    stats->rx_packets = atomic64_read(&total_rx_packets);
    stats->tx_bytes = atomic64_read(&total_tx_bytes);
    stats->rx_bytes = atomic64_read(&total_rx_bytes);
    
    u64 samples = atomic64_read(&latency_samples);
    u64 total = atomic64_read(&total_latency_ns);
    stats->avg_latency_ns = samples > 0 ? total / samples : 0;
    
    stats->num_flows = 0;
    struct virtio_nic_flow_metric *flow;
    unsigned long flags;
    
    spin_lock_irqsave(&flow_metrics_lock, flags);
    list_for_each_entry(flow, &flow_metrics_list, list) {
        stats->num_flows++;
    }
    spin_unlock_irqrestore(&flow_metrics_lock, flags);
}
EXPORT_SYMBOL_GPL(telemetry_get_stats);

/* Module initialization */
static int __init virtio_nic_telemetry_init(void)
{
    return 0;
}

static void __exit virtio_nic_telemetry_exit(void)
{
    telemetry_exit();
}

module_init(virtio_nic_telemetry_init);
module_exit(virtio_nic_telemetry_exit);

MODULE_DESCRIPTION("Advanced telemetry and monitoring for VirtIO NIC driver");
MODULE_LICENSE("GPL");

