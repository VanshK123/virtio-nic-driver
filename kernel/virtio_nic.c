#include <linux/module.h>
#include <linux/slab.h>
#include <linux/virtio_config.h>
#include <linux/numa.h>
#include <linux/cpumask.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/perf_event.h>
#include <linux/ktime.h>
#include "virtio_nic.h"

static const struct net_device_ops virtio_nic_netdev_ops = {
    .ndo_open       = virtio_nic_open,
    .ndo_stop       = virtio_nic_stop,
    .ndo_start_xmit = virtio_nic_start_xmit,
    .ndo_get_stats  = virtio_nic_get_stats,
};

/* Global telemetry instance */
static struct virtio_nic_telemetry *global_telemetry;

/* Performance tuning parameters */
static int num_queues = 32;
static int numa_node = -1;
static int coalesce_usecs = VIRTIO_NIC_COALESCE_USECS;
static bool enable_zero_copy = true;
static bool enable_numa_aware = true;

module_param(num_queues, int, 0644);
module_param(numa_node, int, 0644);
module_param(coalesce_usecs, int, 0644);
module_param(enable_zero_copy, bool, 0644);
module_param(enable_numa_aware, bool, 0644);

MODULE_PARM_DESC(num_queues, "Number of queues (default: 32)");
MODULE_PARM_DESC(numa_node, "NUMA node to bind to (-1 for auto)");
MODULE_PARM_DESC(coalesce_usecs, "Interrupt coalescing time in usecs");
MODULE_PARM_DESC(enable_zero_copy, "Enable zero-copy DMA (default: true)");
MODULE_PARM_DESC(enable_numa_aware, "Enable NUMA-aware scheduling (default: true)");

static int virtio_nic_probe(struct virtio_device *vdev)
{
    struct net_device *ndev;
    struct virtio_nic_priv *priv;
    int err;

    ndev = alloc_etherdev(sizeof(*priv));
    if (!ndev)
        return -ENOMEM;

    priv = netdev_priv(ndev);
    memset(priv, 0, sizeof(*priv));
    priv->vdev = vdev;
    priv->netdev = ndev;
    priv->num_queues = num_queues;
    priv->active_queues = 0;
    priv->numa_node = numa_node;
    
    spin_lock_init(&priv->stats_lock);
    atomic_set(&priv->failover_count, 0);

    ndev->netdev_ops = &virtio_nic_netdev_ops;
    SET_NETDEV_DEV(ndev, &vdev->dev);
    vdev->priv = priv;

    /* Initialize NUMA-aware setup */
    if (enable_numa_aware) {
        err = virtio_nic_numa_setup(priv);
        if (err) {
            dev_err(&vdev->dev, "Failed to setup NUMA awareness: %d\n", err);
            goto free_netdev;
        }
    }

    /* Setup queues with NUMA awareness */
    err = virtio_nic_setup_queues(priv);
    if (err) {
        dev_err(&vdev->dev, "Failed to setup queues: %d\n", err);
        goto cleanup_numa;
    }

    /* Setup MSI-X interrupts */
    err = virtio_nic_setup_msix(priv);
    if (err) {
        dev_err(&vdev->dev, "Failed to setup MSI-X: %d\n", err);
        goto teardown_queues;
    }

    /* Initialize failover mechanism */
    virtio_nic_init_failover(priv);

    /* Initialize telemetry */
    telemetry_init(ndev);

    err = register_netdev(ndev);
    if (err) {
        dev_err(&vdev->dev, "Failed to register netdev: %d\n", err);
        goto cleanup_failover;
    }

    virtio_device_ready(vdev);
    
    dev_info(&vdev->dev, "VirtIO NIC driver initialized with %d queues on NUMA %d\n",
             priv->num_queues, priv->numa_node);
    
    return 0;

cleanup_failover:
    virtio_nic_cleanup_failover(priv);
teardown_queues:
    virtio_nic_teardown_queues(priv);
cleanup_numa:
    if (enable_numa_aware)
        virtio_nic_bind_to_numa(priv, -1);
free_netdev:
    free_netdev(ndev);
    return err;
}

static void virtio_nic_remove(struct virtio_device *vdev)
{
    struct virtio_nic_priv *priv = vdev->priv;

    if (!priv)
        return;

    telemetry_exit();
    virtio_nic_cleanup_failover(priv);
    virtio_nic_free_irqs(priv);
    virtio_nic_teardown_queues(priv);
    if (enable_numa_aware)
        virtio_nic_bind_to_numa(priv, -1);
    unregister_netdev(priv->netdev);
    free_netdev(priv->netdev);
}

int virtio_nic_open(struct net_device *ndev)
{
    struct virtio_nic_priv *priv = netdev_priv(ndev);
    int i;

    /* Start adaptive scheduling */
    if (enable_numa_aware)
        virtio_nic_adaptive_scheduling(priv);

    /* Enable all queues */
    for (i = 0; i < priv->num_queues; i++) {
        napi_enable(&priv->queues[i].napi);
        netif_napi_add(ndev, &priv->queues[i].napi, virtio_nic_poll, VIRTIO_NIC_NAPI_WEIGHT);
    }

    netif_start_queue(ndev);
    return 0;
}

int virtio_nic_stop(struct net_device *ndev)
{
    struct virtio_nic_priv *priv = netdev_priv(ndev);
    int i;

    netif_stop_queue(ndev);

    /* Disable all queues */
    for (i = 0; i < priv->num_queues; i++) {
        netif_napi_del(&priv->queues[i].napi);
        napi_disable(&priv->queues[i].napi);
    }

    return 0;
}

netdev_tx_t virtio_nic_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
    struct virtio_nic_priv *priv = netdev_priv(ndev);
    struct virtio_nic_queue *q;
    struct scatterlist sg[16];
    int nents, err;
    ktime_t start_time;
    u32 flow_id;

    start_time = ktime_get();
    
    /* Extract flow ID for QoS and failover */
    flow_id = skb->hash ? skb->hash % priv->num_queues : 0;
    q = &priv->queues[flow_id % priv->active_queues];

    if (enable_zero_copy) {
        /* Zero-copy DMA mapping */
        err = virtio_nic_dma_map_skb(skb, sg, &nents);
        if (err) {
            dev_kfree_skb_any(skb);
            return NETDEV_TX_BUSY;
        }
    } else {
        /* Traditional scatter-gather */
        sg_init_table(sg, 1);
        sg_set_buf(sg, skb->data, skb->len);
        nents = 1;
    }

    err = virtio_nic_enqueue(q, sg, 1, 0, skb);
    if (err) {
        dev_kfree_skb_any(skb);
        return NETDEV_TX_BUSY;
    }

    /* Update statistics */
    spin_lock(&priv->stats_lock);
    priv->total_tx_packets++;
    priv->total_tx_bytes += skb->len;
    q->tx_packets++;
    q->tx_bytes += skb->len;
    spin_unlock(&priv->stats_lock);

    /* Record latency for telemetry */
    telemetry_record_latency(ktime_to_ns(ktime_sub(ktime_get(), start_time)));
    telemetry_record_tx();

    return NETDEV_TX_OK;
}

/* NAPI poll function for efficient packet processing */
int virtio_nic_poll(struct napi_struct *napi, int budget)
{
    struct virtio_nic_queue *q = container_of(napi, struct virtio_nic_queue, napi);
    struct virtio_nic_priv *priv = netdev_priv(q->vq->vdev->priv);
    unsigned int len;
    void *buf;
    int work_done = 0;

    while (work_done < budget) {
        buf = virtio_nic_dequeue(q, &len);
        if (!buf)
            break;

        /* Process received packet */
        if (len > 0) {
            struct sk_buff *skb = (struct sk_buff *)buf;
            netif_receive_skb(skb);
            work_done++;
            
            /* Update statistics */
            spin_lock(&priv->stats_lock);
            priv->total_rx_packets++;
            priv->total_rx_bytes += len;
            q->rx_packets++;
            q->rx_bytes += len;
            spin_unlock(&priv->stats_lock);
            
            telemetry_record_rx();
        }
    }

    if (work_done < budget) {
        napi_complete(napi);
        virtqueue_enable_cb(q->vq);
    }

    return work_done;
}

/* Enhanced statistics collection */
struct net_device_stats *virtio_nic_get_stats(struct net_device *ndev)
{
    struct virtio_nic_priv *priv = netdev_priv(ndev);
    struct net_device_stats *stats = &ndev->stats;
    int i;

    spin_lock(&priv->stats_lock);
    stats->rx_packets = priv->total_rx_packets;
    stats->tx_packets = priv->total_tx_packets;
    stats->rx_bytes = priv->total_rx_bytes;
    stats->tx_bytes = priv->total_tx_bytes;
    
    for (i = 0; i < priv->num_queues; i++) {
        stats->rx_errors += priv->queues[i].rx_errors;
        stats->tx_errors += priv->queues[i].tx_errors;
        stats->rx_dropped += priv->queues[i].rx_dropped;
        stats->tx_dropped += priv->queues[i].tx_dropped;
    }
    spin_unlock(&priv->stats_lock);

    return stats;
}

static const struct virtio_device_id id_table[] = {
    { VIRTIO_ID_NET, VIRTIO_DEV_ANY_ID },
    { 0 }
};
MODULE_DEVICE_TABLE(virtio, id_table);

static struct virtio_driver virtio_nic_driver = {
    .driver.name = "virtio_nic",
    .driver.owner = THIS_MODULE,
    .id_table = id_table,
    .probe = virtio_nic_probe,
    .remove = virtio_nic_remove,
};

int __init virtio_nic_init(void)
{
    return register_virtio_driver(&virtio_nic_driver);
}

void __exit virtio_nic_exit(void)
{
    unregister_virtio_driver(&virtio_nic_driver);
}

module_init(virtio_nic_init);
module_exit(virtio_nic_exit);

MODULE_DESCRIPTION("High-performance VirtIO NIC driver with zero-copy DMA and NUMA awareness");
MODULE_LICENSE("GPL");
