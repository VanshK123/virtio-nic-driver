#include <linux/module.h>
#include <linux/slab.h>
#include <linux/virtio_config.h>
#include <linux/numa.h>
#include <linux/cpumask.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include "virtio_nic.h"

/* Queue scheduling parameters */
static int queue_weight = 64;
static int adaptive_threshold = 1000; /* packets per second */
static bool enable_adaptive_scheduling = true;

module_param(queue_weight, int, 0644);
module_param(adaptive_threshold, int, 0644);
module_param(enable_adaptive_scheduling, bool, 0644);

MODULE_PARM_DESC(queue_weight, "NAPI weight for queue processing");
MODULE_PARM_DESC(adaptive_threshold, "Threshold for adaptive scheduling");
MODULE_PARM_DESC(enable_adaptive_scheduling, "Enable adaptive queue scheduling");

/* NUMA-aware queue setup */
int virtio_nic_setup_queues(struct virtio_nic_priv *priv)
{
    struct virtqueue *vqs[VIRTIO_NIC_MAX_QUEUES];
    static const char * const names[] = { "rx", "tx" };
    int i, err, numa_nodes = num_possible_nodes();
    int queues_per_numa;

    if (!priv || !priv->vdev)
        return -EINVAL;

    /* Allocate queue structures */
    priv->queues = kcalloc(priv->num_queues, sizeof(struct virtio_nic_queue), GFP_KERNEL);
    if (!priv->queues)
        return -ENOMEM;

    /* Setup virtqueues */
    err = virtio_find_vqs(priv->vdev, priv->num_queues, vqs, NULL, names, NULL);
    if (err) {
        kfree(priv->queues);
        return err;
    }

    /* Initialize queues with NUMA awareness */
    queues_per_numa = priv->num_queues / numa_nodes;
    for (i = 0; i < priv->num_queues; i++) {
        struct virtio_nic_queue *q = &priv->queues[i];
        int numa_node = i / queues_per_numa;

        q->vq = vqs[i];
        q->numa_node = numa_node;
        q->cpu_id = -1; /* Will be assigned during NUMA setup */
        q->flow_tag = i;
        q->irq = -1;

        /* Initialize statistics */
        q->rx_bytes = 0;
        q->tx_bytes = 0;
        q->rx_packets = 0;
        q->tx_packets = 0;
        q->rx_errors = 0;
        q->tx_errors = 0;
        q->rx_dropped = 0;
        q->tx_dropped = 0;

        /* Initialize locks and lists */
        spin_lock_init(&q->lock);
        spin_lock_init(&q->flow_lock);
        INIT_LIST_HEAD(&q->flow_list);
        atomic_set(&q->pending_packets, 0);

        /* Setup NAPI */
        netif_napi_add(priv->netdev, &q->napi, virtio_nic_poll, queue_weight);

        /* Setup coalescing timer */
        setup_timer(&q->coalesce_timer, virtio_nic_coalesce_timer, (unsigned long)q);

        /* Setup failover work */
        INIT_WORK(&q->failover_work, virtio_nic_failover_work);
    }

    priv->active_queues = priv->num_queues;
    return 0;
}
EXPORT_SYMBOL_GPL(virtio_nic_setup_queues);

void virtio_nic_teardown_queues(struct virtio_nic_priv *priv)
{
    int i;

    if (!priv || !priv->queues)
        return;

    for (i = 0; i < priv->num_queues; i++) {
        struct virtio_nic_queue *q = &priv->queues[i];

        /* Cancel timers and work */
        del_timer_sync(&q->coalesce_timer);
        cancel_work_sync(&q->failover_work);

        /* Cleanup NAPI */
        netif_napi_del(&q->napi);

        /* Free flow list */
        virtio_nic_cleanup_flow_list(q);
    }

    priv->vdev->config->del_vqs(priv->vdev);
    kfree(priv->queues);
    priv->queues = NULL;
    priv->num_queues = 0;
    priv->active_queues = 0;
}
EXPORT_SYMBOL_GPL(virtio_nic_teardown_queues);

/* Enhanced enqueue with flow tracking */
int virtio_nic_enqueue(struct virtio_nic_queue *q, struct scatterlist *sg,
                       unsigned int out, unsigned int in, void *data)
{
    unsigned long flags;
    int err;
    struct sk_buff *skb = (struct sk_buff *)data;
    u32 flow_id;

    if (!q || !sg)
        return -EINVAL;

    /* Extract flow ID for tracking */
    flow_id = skb ? (skb->hash % 0xFFFF) : 0;

    spin_lock_irqsave(&q->lock, flags);
    
    err = virtqueue_add_sgs(q->vq, sg, out, in, data, GFP_ATOMIC);
    if (!err) {
        virtqueue_kick(q->vq);
        atomic_inc(&q->pending_packets);
        
        /* Update flow tracking */
        virtio_nic_update_flow_stats(q, flow_id, skb ? skb->len : 0);
    }

    spin_unlock_irqrestore(&q->lock, flags);
    return err;
}
EXPORT_SYMBOL_GPL(virtio_nic_enqueue);

/* Enhanced dequeue with statistics */
void *virtio_nic_dequeue(struct virtio_nic_queue *q, unsigned int *len)
{
    unsigned long flags;
    void *buf;

    if (!q || !len)
        return NULL;

    spin_lock_irqsave(&q->lock, flags);
    buf = virtqueue_get_buf(q->vq, len);
    if (buf) {
        atomic_dec(&q->pending_packets);
        telemetry_record_rx();
    }
    spin_unlock_irqrestore(&q->lock, flags);

    return buf;
}
EXPORT_SYMBOL_GPL(virtio_nic_dequeue);

/* Assign queue to specific CPU for NUMA optimization */
int virtio_nic_assign_queue_to_cpu(struct virtio_nic_queue *q, int cpu)
{
    if (!q || cpu < 0 || cpu >= num_possible_cpus())
        return -EINVAL;

    q->cpu_id = cpu;
    
    /* Bind NAPI to specific CPU */
    if (q->napi.poll)
        netif_napi_add_cpu(q->napi.dev, &q->napi, virtio_nic_poll, queue_weight, cpu);

    return 0;
}
EXPORT_SYMBOL_GPL(virtio_nic_assign_queue_to_cpu);

/* Adaptive queue scheduling based on load */
void virtio_nic_adaptive_scheduling(struct virtio_nic_priv *priv)
{
    int i, total_load = 0;
    int queues_per_cpu = priv->num_queues / num_possible_cpus();

    if (!enable_adaptive_scheduling)
        return;

    /* Calculate total load across all queues */
    for (i = 0; i < priv->num_queues; i++) {
        total_load += atomic_read(&priv->queues[i].pending_packets);
    }

    /* If load is high, adjust queue assignments */
    if (total_load > adaptive_threshold) {
        for (i = 0; i < priv->num_queues; i++) {
            struct virtio_nic_queue *q = &priv->queues[i];
            int load = atomic_read(&q->pending_packets);
            
            /* Reassign busy queues to less loaded CPUs */
            if (load > adaptive_threshold / priv->num_queues) {
                int new_cpu = (i + 1) % num_possible_cpus();
                virtio_nic_assign_queue_to_cpu(q, new_cpu);
            }
        }
    }
}
EXPORT_SYMBOL_GPL(virtio_nic_adaptive_scheduling);

/* Coalescing timer callback */
void virtio_nic_coalesce_timer(unsigned long data)
{
    struct virtio_nic_queue *q = (struct virtio_nic_queue *)data;
    
    if (q && q->napi.poll) {
        napi_schedule(&q->napi);
    }
}

/* Failover work function */
void virtio_nic_failover_work(struct work_struct *work)
{
    struct virtio_nic_queue *q = container_of(work, struct virtio_nic_queue, failover_work);
    struct virtio_nic_priv *priv = netdev_priv(q->vq->vdev->priv);
    
    /* Implement queue failover logic */
    if (q->rx_errors > 1000 || q->tx_errors > 1000) {
        /* Mark queue as failed and reassign flows */
        dev_warn(&priv->vdev->dev, "Queue %d failed, reassigning flows\n", q->flow_tag);
        virtio_nic_remap_queue(priv, q->flow_tag, (q->flow_tag + 1) % priv->num_queues);
    }
}

/* Update flow statistics */
void virtio_nic_update_flow_stats(struct virtio_nic_queue *q, u32 flow_id, u32 bytes)
{
    struct virtio_nic_flow *flow;
    unsigned long flags;

    spin_lock_irqsave(&q->flow_lock, flags);
    
    /* Find or create flow entry */
    list_for_each_entry(flow, &q->flow_list, list) {
        if (flow->flow_id == flow_id) {
            flow->bytes += bytes;
            flow->packets++;
            flow->last_seen = jiffies;
            goto out;
        }
    }

    /* Create new flow entry */
    flow = kzalloc(sizeof(*flow), GFP_ATOMIC);
    if (flow) {
        flow->flow_id = flow_id;
        flow->queue_id = q->flow_tag;
        flow->bytes = bytes;
        flow->packets = 1;
        flow->last_seen = jiffies;
        list_add_tail(&flow->list, &q->flow_list);
    }

out:
    spin_unlock_irqrestore(&q->flow_lock, flags);
}
EXPORT_SYMBOL_GPL(virtio_nic_update_flow_stats);

/* Cleanup flow list */
void virtio_nic_cleanup_flow_list(struct virtio_nic_queue *q)
{
    struct virtio_nic_flow *flow, *tmp;
    unsigned long flags;

    spin_lock_irqsave(&q->flow_lock, flags);
    list_for_each_entry_safe(flow, tmp, &q->flow_list, list) {
        list_del(&flow->list);
        kfree(flow);
    }
    spin_unlock_irqrestore(&q->flow_lock, flags);
}
EXPORT_SYMBOL_GPL(virtio_nic_cleanup_flow_list);

/* Get queue statistics */
void virtio_nic_get_queue_stats(struct virtio_nic_queue *q, struct virtio_nic_queue_stats *stats)
{
    if (!q || !stats)
        return;

    stats->rx_bytes = q->rx_bytes;
    stats->tx_bytes = q->tx_bytes;
    stats->rx_packets = q->rx_packets;
    stats->tx_packets = q->tx_packets;
    stats->rx_errors = q->rx_errors;
    stats->tx_errors = q->tx_errors;
    stats->pending_packets = atomic_read(&q->pending_packets);
    stats->numa_node = q->numa_node;
    stats->cpu_id = q->cpu_id;
}
EXPORT_SYMBOL_GPL(virtio_nic_get_queue_stats);

/* Module initialization */
static int __init virtio_nic_queue_init(void)
{
    return 0;
}

static void __exit virtio_nic_queue_exit(void)
{
    /* Cleanup handled by main driver */
}

module_init(virtio_nic_queue_init);
module_exit(virtio_nic_queue_exit);

MODULE_DESCRIPTION("NUMA-aware queue management for VirtIO NIC driver");
MODULE_LICENSE("GPL");
