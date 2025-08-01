#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/ktime.h>
#include <linux/numa.h>
#include <linux/cpumask.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include "virtio_nic.h"

/* Failover configuration */
static int failover_enabled = true;
static int health_check_interval_ms = 1000;
static int max_failover_count = 3;
static int queue_failure_threshold = 1000;

module_param(failover_enabled, bool, 0644);
module_param(health_check_interval_ms, int, 0644);
module_param(max_failover_count, int, 0644);
module_param(queue_failure_threshold, int, 0644);

MODULE_PARM_DESC(failover_enabled, "Enable failover mechanism");
MODULE_PARM_DESC(health_check_interval_ms, "Health check interval in milliseconds");
MODULE_PARM_DESC(max_failover_count, "Maximum failover attempts");
MODULE_PARM_DESC(queue_failure_threshold, "Queue failure threshold");

/* Failover state tracking */
struct virtio_nic_failover_state {
    atomic_t failover_count;
    atomic_t active_queues;
    atomic_t failed_queues;
    spinlock_t failover_lock;
    struct timer_list health_check_timer;
    struct workqueue_struct *failover_wq;
    struct list_head failed_queue_list;
    spinlock_t failed_queue_lock;
};

/* Failed queue tracking */
struct virtio_nic_failed_queue {
    int queue_id;
    int failure_count;
    ktime_t last_failure;
    ktime_t recovery_time;
    struct list_head list;
};

/* Initialize failover mechanism */
void virtio_nic_init_failover(struct virtio_nic_priv *priv)
{
    struct virtio_nic_failover_state *failover;
    
    if (!priv || !failover_enabled)
        return;

    failover = kzalloc(sizeof(*failover), GFP_KERNEL);
    if (!failover)
        return;

    atomic_set(&failover->failover_count, 0);
    atomic_set(&failover->active_queues, priv->num_queues);
    atomic_set(&failover->failed_queues, 0);
    spin_lock_init(&failover->failover_lock);
    spin_lock_init(&failover->failed_queue_lock);
    INIT_LIST_HEAD(&failover->failed_queue_list);

    /* Create workqueue for failover operations */
    failover->failover_wq = create_singlethread_workqueue("virtio_nic_failover");
    if (!failover->failover_wq) {
        kfree(failover);
        return;
    }

    /* Setup health check timer */
    setup_timer(&failover->health_check_timer, virtio_nic_health_check_timer, (unsigned long)priv);
    mod_timer(&failover->health_check_timer, 
              jiffies + msecs_to_jiffies(health_check_interval_ms));

    priv->failover_state = failover;
}
EXPORT_SYMBOL_GPL(virtio_nic_init_failover);

/* Cleanup failover mechanism */
void virtio_nic_cleanup_failover(struct virtio_nic_priv *priv)
{
    struct virtio_nic_failover_state *failover = priv->failover_state;
    struct virtio_nic_failed_queue *failed_q, *tmp;

    if (!failover)
        return;

    /* Cancel health check timer */
    del_timer_sync(&failover->health_check_timer);

    /* Cleanup failed queue list */
    spin_lock(&failover->failed_queue_lock);
    list_for_each_entry_safe(failed_q, tmp, &failover->failed_queue_list, list) {
        list_del(&failed_q->list);
        kfree(failed_q);
    }
    spin_unlock(&failover->failed_queue_lock);

    /* Destroy workqueue */
    if (failover->failover_wq) {
        destroy_workqueue(failover->failover_wq);
    }

    kfree(failover);
    priv->failover_state = NULL;
}
EXPORT_SYMBOL_GPL(virtio_nic_cleanup_failover);

/* Health check timer callback */
void virtio_nic_health_check_timer(unsigned long data)
{
    struct virtio_nic_priv *priv = (struct virtio_nic_priv *)data;
    struct virtio_nic_failover_state *failover = priv->failover_state;
    int i;

    if (!failover || !failover_enabled)
        return;

    /* Check all queues for failures */
    for (i = 0; i < priv->num_queues; i++) {
        struct virtio_nic_queue *q = &priv->queues[i];
        
        /* Check for excessive errors */
        if (q->rx_errors > queue_failure_threshold || 
            q->tx_errors > queue_failure_threshold) {
            
            /* Queue has failed, trigger failover */
            virtio_nic_queue_failed(priv, i);
        }
    }

    /* Reschedule timer */
    mod_timer(&failover->health_check_timer, 
              jiffies + msecs_to_jiffies(health_check_interval_ms));
}

/* Handle queue failure */
void virtio_nic_queue_failed(struct virtio_nic_priv *priv, int queue_id)
{
    struct virtio_nic_failover_state *failover = priv->failover_state;
    struct virtio_nic_failed_queue *failed_q;
    unsigned long flags;

    if (!failover || queue_id >= priv->num_queues)
        return;

    spin_lock_irqsave(&failover->failed_queue_lock, flags);
    
    /* Check if queue is already marked as failed */
    list_for_each_entry(failed_q, &failover->failed_queue_list, list) {
        if (failed_q->queue_id == queue_id) {
            failed_q->failure_count++;
            failed_q->last_failure = ktime_get();
            goto out;
        }
    }

    /* Create new failed queue entry */
    failed_q = kzalloc(sizeof(*failed_q), GFP_ATOMIC);
    if (failed_q) {
        failed_q->queue_id = queue_id;
        failed_q->failure_count = 1;
        failed_q->last_failure = ktime_get();
        list_add_tail(&failed_q->list, &failover->failed_queue_list);
        
        atomic_inc(&failover->failed_queues);
        atomic_dec(&failover->active_queues);
    }

out:
    spin_unlock_irqrestore(&failover->failed_queue_lock, flags);

    /* Trigger queue remapping */
    if (atomic_read(&failover->failover_count) < max_failover_count) {
        atomic_inc(&failover->failover_count);
        virtio_nic_remap_queue(priv, queue_id, -1);
    }
}

/* Remap queue to new target */
int virtio_nic_remap_queue(struct virtio_nic_priv *priv, int old_queue, int new_queue)
{
    struct virtio_nic_queue *q;
    int target_queue;

    if (!priv || old_queue >= priv->num_queues)
        return -EINVAL;

    /* Find available queue if new_queue is -1 */
    if (new_queue == -1) {
        target_queue = virtio_nic_find_available_queue(priv);
        if (target_queue < 0)
            return -ENOMEM;
    } else {
        target_queue = new_queue;
    }

    q = &priv->queues[old_queue];

    /* Reassign flows from failed queue to new queue */
    virtio_nic_reassign_queue_flows(priv, old_queue, target_queue);

    /* Update queue statistics */
    q->rx_errors = 0;
    q->tx_errors = 0;

    dev_info(&priv->vdev->dev, "Queue %d remapped to queue %d\n", old_queue, target_queue);

    return 0;
}
EXPORT_SYMBOL_GPL(virtio_nic_remap_queue);

/* Find available queue for failover */
int virtio_nic_find_available_queue(struct virtio_nic_priv *priv)
{
    int i;
    struct virtio_nic_failover_state *failover = priv->failover_state;

    if (!failover)
        return -1;

    /* Find queue with lowest error rate */
    int best_queue = -1;
    u64 min_errors = U64_MAX;

    for (i = 0; i < priv->num_queues; i++) {
        struct virtio_nic_queue *q = &priv->queues[i];
        u64 total_errors = q->rx_errors + q->tx_errors;
        
        if (total_errors < min_errors) {
            min_errors = total_errors;
            best_queue = i;
        }
    }

    return best_queue;
}
EXPORT_SYMBOL_GPL(virtio_nic_find_available_queue);

/* Reassign flows from one queue to another */
void virtio_nic_reassign_queue_flows(struct virtio_nic_priv *priv, int old_queue, int new_queue)
{
    struct virtio_nic_queue *old_q = &priv->queues[old_queue];
    struct virtio_nic_queue *new_q = &priv->queues[new_queue];
    struct virtio_nic_flow *flow, *tmp;
    unsigned long flags;

    if (!old_q || !new_q)
        return;

    spin_lock_irqsave(&old_q->flow_lock, flags);
    
    list_for_each_entry_safe(flow, tmp, &old_q->flow_list, list) {
        /* Move flow to new queue */
        list_del(&flow->list);
        flow->queue_id = new_queue;
        
        /* Add to new queue's flow list */
        spin_lock(&new_q->flow_lock);
        list_add_tail(&flow->list, &new_q->flow_list);
        spin_unlock(&new_q->flow_lock);
    }
    
    spin_unlock_irqrestore(&old_q->flow_lock, flags);
}
EXPORT_SYMBOL_GPL(virtio_nic_reassign_queue_flows);

/* Reassign specific flow to new queue */
void virtio_nic_flow_reassign(struct virtio_nic_priv *priv, u32 flow_id, int new_queue)
{
    int i;
    struct virtio_nic_flow *flow;

    if (!priv || new_queue >= priv->num_queues)
        return;

    /* Find flow in all queues */
    for (i = 0; i < priv->num_queues; i++) {
        struct virtio_nic_queue *q = &priv->queues[i];
        unsigned long flags;

        spin_lock_irqsave(&q->flow_lock, flags);
        list_for_each_entry(flow, &q->flow_list, list) {
            if (flow->flow_id == flow_id) {
                /* Move flow to new queue */
                list_del(&flow->list);
                flow->queue_id = new_queue;
                
                /* Add to new queue */
                struct virtio_nic_queue *new_q = &priv->queues[new_queue];
                spin_lock(&new_q->flow_lock);
                list_add_tail(&flow->list, &new_q->flow_list);
                spin_unlock(&new_q->flow_lock);
                
                spin_unlock_irqrestore(&q->flow_lock, flags);
                return;
            }
        }
        spin_unlock_irqrestore(&q->flow_lock, flags);
    }
}
EXPORT_SYMBOL_GPL(virtio_nic_flow_reassign);

/* Get failover statistics */
void virtio_nic_get_failover_stats(struct virtio_nic_priv *priv, struct virtio_nic_failover_stats *stats)
{
    struct virtio_nic_failover_state *failover = priv->failover_state;
    struct virtio_nic_failed_queue *failed_q;

    if (!failover || !stats)
        return;

    memset(stats, 0, sizeof(*stats));
    
    stats->failover_count = atomic_read(&failover->failover_count);
    stats->active_queues = atomic_read(&failover->active_queues);
    stats->failed_queues = atomic_read(&failover->failed_queues);
    stats->enabled = failover_enabled;
    
    /* Count failed queues */
    spin_lock(&failover->failed_queue_lock);
    list_for_each_entry(failed_q, &failover->failed_queue_list, list) {
        stats->total_failures++;
        if (failed_q->failure_count > stats->max_failure_count) {
            stats->max_failure_count = failed_q->failure_count;
        }
    }
    spin_unlock(&failover->failed_queue_lock);
}
EXPORT_SYMBOL_GPL(virtio_nic_get_failover_stats);

/* Recovery mechanism for failed queues */
void virtio_nic_queue_recovery_work(struct work_struct *work)
{
    struct virtio_nic_priv *priv = container_of(work, struct virtio_nic_priv, recovery_work);
    struct virtio_nic_failover_state *failover = priv->failover_state;
    struct virtio_nic_failed_queue *failed_q, *tmp;
    unsigned long flags;
    ktime_t now = ktime_get();

    if (!failover)
        return;

    spin_lock_irqsave(&failover->failed_queue_lock, flags);
    
    list_for_each_entry_safe(failed_q, tmp, &failover->failed_queue_list, list) {
        /* Check if queue has recovered */
        if (ktime_to_ms(ktime_sub(now, failed_q->last_failure)) > 5000) { /* 5 second recovery window */
            struct virtio_nic_queue *q = &priv->queues[failed_q->queue_id];
            
            /* Reset error counters */
            q->rx_errors = 0;
            q->tx_errors = 0;
            
            /* Remove from failed list */
            list_del(&failed_q->list);
            kfree(failed_q);
            
            atomic_dec(&failover->failed_queues);
            atomic_inc(&failover->active_queues);
            
            dev_info(&priv->vdev->dev, "Queue %d recovered\n", failed_q->queue_id);
        }
    }
    
    spin_unlock_irqrestore(&failover->failed_queue_lock, flags);
}
EXPORT_SYMBOL_GPL(virtio_nic_queue_recovery_work);

/* Module initialization */
static int __init virtio_nic_failover_init(void)
{
    return 0;
}

static void __exit virtio_nic_failover_exit(void)
{
    /* Cleanup handled by main driver */
}

module_init(virtio_nic_failover_init);
module_exit(virtio_nic_failover_exit);

MODULE_DESCRIPTION("Failover and resilience for VirtIO NIC driver");
MODULE_LICENSE("GPL"); 