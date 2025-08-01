#ifndef VIRTIO_NIC_H
#define VIRTIO_NIC_H

#include <linux/netdevice.h>
#include <linux/virtio.h>
#include <linux/virtio_net.h>
#include <linux/pci.h>
#include <linux/numa.h>
#include <linux/cpumask.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/perf_event.h>

/* Performance tuning constants */
#define VIRTIO_NIC_MAX_QUEUES 32
#define VIRTIO_NIC_DMA_CHUNK_SIZE (64 * 1024)  /* 64KB DMA chunks */
#define VIRTIO_NIC_COALESCE_USECS 64
#define VIRTIO_NIC_NAPI_WEIGHT 64

/* Flow tracking for QoS and failover */
struct virtio_nic_flow {
    u32 flow_id;
    u32 queue_id;
    u64 bytes;
    u64 packets;
    u64 last_seen;
    struct list_head list;
};

/* Enhanced queue structure with NUMA awareness */
struct virtio_nic_queue {
    struct virtqueue *vq;
    struct napi_struct napi;
    spinlock_t lock;
    u32 flow_tag;
    int irq;
    int numa_node;
    int cpu_id;
    atomic_t pending_packets;
    struct timer_list coalesce_timer;
    struct work_struct failover_work;
    struct list_head flow_list;
    spinlock_t flow_lock;
    u64 rx_bytes;
    u64 tx_bytes;
    u64 rx_packets;
    u64 tx_packets;
    u64 rx_errors;
    u64 tx_errors;
    u64 rx_dropped;
    u64 tx_dropped;
    struct perf_event *perf_event;
};

/* Zero-copy DMA buffer management */
struct virtio_nic_dma_buf {
    struct page **pages;
    struct scatterlist *sgl;
    unsigned int nents;
    unsigned int nr_pages;
    dma_addr_t dma_addr;
    size_t size;
    bool write;
};

/* Enhanced private data structure */
struct virtio_nic_priv {
    struct virtio_device *vdev;
    struct net_device *netdev;
    struct virtio_nic_queue *queues;
    unsigned int num_queues;
    unsigned int active_queues;
    int numa_node;
    struct cpumask cpu_mask;
    struct workqueue_struct *failover_wq;
    struct timer_list health_check_timer;
    atomic_t failover_count;
    u64 total_rx_bytes;
    u64 total_tx_bytes;
    u64 total_rx_packets;
    u64 total_tx_packets;
    spinlock_t stats_lock;
};

/* Telemetry and monitoring */
struct virtio_nic_telemetry {
    struct perf_event *tx_event;
    struct perf_event *rx_event;
    struct perf_event *latency_event;
    struct kobject *kobj;
    struct kobj_attribute tx_attr;
    struct kobj_attribute rx_attr;
    struct kobj_attribute latency_attr;
    struct kobj_attribute throughput_attr;
    struct kobj_attribute queue_stats_attr;
    struct kobj_attribute flow_stats_attr;
    spinlock_t lock;
};

/* Function declarations */
int virtio_nic_init(void);
void virtio_nic_exit(void);
int virtio_nic_open(struct net_device *ndev);
int virtio_nic_stop(struct net_device *ndev);
netdev_tx_t virtio_nic_start_xmit(struct sk_buff *skb, struct net_device *ndev);

/* Zero-copy DMA functions */
int virtio_nic_dma_alloc_buffer(struct virtio_nic_dma_buf *buf, size_t size, bool write);
void virtio_nic_dma_free_buffer(struct virtio_nic_dma_buf *buf);
int virtio_nic_dma_map_skb(struct sk_buff *skb, struct scatterlist *sg, int *nents);

/* Queue management with NUMA awareness */
int virtio_nic_setup_queues(struct virtio_nic_priv *priv);
void virtio_nic_teardown_queues(struct virtio_nic_priv *priv);
int virtio_nic_enqueue(struct virtio_nic_queue *q, struct scatterlist *sg,
                       unsigned int out, unsigned int in, void *data);
void *virtio_nic_dequeue(struct virtio_nic_queue *q, unsigned int *len);
int virtio_nic_assign_queue_to_cpu(struct virtio_nic_queue *q, int cpu);

/* MSI-X and interrupt management */
int virtio_nic_request_irqs(struct virtio_nic_priv *priv);
void virtio_nic_free_irqs(struct virtio_nic_priv *priv);
void virtio_nic_update_coalesce(int usecs);
int virtio_nic_setup_msix(struct virtio_nic_priv *priv);

/* Failover and resilience */
void virtio_nic_init_failover(struct virtio_nic_priv *priv);
void virtio_nic_cleanup_failover(struct virtio_nic_priv *priv);
int virtio_nic_remap_queue(struct virtio_nic_priv *priv, int old_queue, int new_queue);
void virtio_nic_flow_reassign(struct virtio_nic_priv *priv, u32 flow_id, int new_queue);

/* Telemetry and monitoring */
void telemetry_init(struct net_device *ndev);
void telemetry_exit(void);
void telemetry_record_tx(void);
void telemetry_record_rx(void);
void telemetry_record_latency(u64 latency_ns);
void telemetry_update_queue_stats(struct virtio_nic_queue *q);
void telemetry_update_flow_stats(struct virtio_nic_flow *flow);

/* NUMA-aware scheduling */
int virtio_nic_numa_setup(struct virtio_nic_priv *priv);
int virtio_nic_bind_to_numa(struct virtio_nic_priv *priv, int numa_node);
void virtio_nic_adaptive_scheduling(struct virtio_nic_priv *priv);

#endif /* VIRTIO_NIC_H */
