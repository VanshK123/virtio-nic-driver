#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/numa.h>
#include <linux/cpumask.h>
#include <linux/ktime.h>
#include <linux/timer.h>
#include "virtio_nic.h"

/* Interrupt coalescing parameters */
static int coalesce_usecs = VIRTIO_NIC_COALESCE_USECS;
static int adaptive_coalesce = true;
static int max_coalesce_usecs = 128;
static int min_coalesce_usecs = 8;

module_param(coalesce_usecs, int, 0644);
module_param(adaptive_coalesce, bool, 0644);
module_param(max_coalesce_usecs, int, 0644);
module_param(min_coalesce_usecs, int, 0644);

MODULE_PARM_DESC(coalesce_usecs, "Interrupt coalescing time in usecs");
MODULE_PARM_DESC(adaptive_coalesce, "Enable adaptive interrupt coalescing");
MODULE_PARM_DESC(max_coalesce_usecs, "Maximum coalescing time in usecs");
MODULE_PARM_DESC(min_coalesce_usecs, "Minimum coalescing time in usecs");

/* Enhanced interrupt handler with latency tracking */
static irqreturn_t virtio_nic_interrupt(int irq, void *data)
{
    struct virtio_nic_queue *q = data;
    ktime_t start_time;
    u64 latency_ns;

    if (!q)
        return IRQ_NONE;

    start_time = ktime_get();

    /* Check if we should disable callbacks */
    if (virtqueue_disable_cb(q->vq)) {
        /* Schedule NAPI for packet processing */
        napi_schedule(&q->napi);
        
        /* Record interrupt latency for telemetry */
        latency_ns = ktime_to_ns(ktime_sub(ktime_get(), start_time));
        telemetry_record_latency(latency_ns);
    }

    return IRQ_HANDLED;
}

/* Setup MSI-X interrupts with NUMA awareness */
int virtio_nic_setup_msix(struct virtio_nic_priv *priv)
{
    struct pci_dev *pdev = to_pci_dev(priv->vdev->dev.parent);
    int i, err, numa_nodes = num_possible_nodes();
    int vectors_per_numa;

    if (!priv || !pdev)
        return -EINVAL;

    /* Request MSI-X vectors */
    err = pci_alloc_irq_vectors(pdev, priv->num_queues, priv->num_queues,
                                PCI_IRQ_MSIX);
    if (err < 0) {
        dev_err(&priv->vdev->dev, "Failed to allocate MSI-X vectors: %d\n", err);
        return err;
    }

    /* Distribute interrupts across NUMA nodes */
    vectors_per_numa = priv->num_queues / numa_nodes;
    for (i = 0; i < priv->num_queues; i++) {
        struct virtio_nic_queue *q = &priv->queues[i];
        int numa_node = i / vectors_per_numa;
        int cpu;

        /* Get CPU from NUMA node */
        cpu = cpumask_first(cpumask_of_node(numa_node));
        if (cpu >= num_possible_cpus())
            cpu = 0;

        /* Assign IRQ */
        q->irq = pci_irq_vector(pdev, i);
        
        /* Request interrupt with NUMA-aware affinity */
        err = request_irq(q->irq, virtio_nic_interrupt, IRQF_SHARED,
                          "virtio_nic", q);
        if (err) {
            dev_err(&priv->vdev->dev, "Failed to request IRQ %d: %d\n", q->irq, err);
            goto cleanup_irqs;
        }

        /* Set interrupt affinity to NUMA-local CPU */
        irq_set_affinity_hint(q->irq, cpumask_of(cpu));
        
        /* Assign queue to CPU for NUMA optimization */
        virtio_nic_assign_queue_to_cpu(q, cpu);

        /* Setup adaptive coalescing timer */
        if (adaptive_coalesce) {
            setup_timer(&q->coalesce_timer, virtio_nic_coalesce_timer, (unsigned long)q);
            mod_timer(&q->coalesce_timer, jiffies + usecs_to_jiffies(coalesce_usecs));
        }
    }

    dev_info(&priv->vdev->dev, "MSI-X setup complete with %d vectors\n", priv->num_queues);
    return 0;

cleanup_irqs:
    /* Cleanup already requested IRQs */
    for (i = 0; i < priv->num_queues; i++) {
        if (priv->queues[i].irq > 0) {
            free_irq(priv->queues[i].irq, &priv->queues[i]);
            priv->queues[i].irq = -1;
        }
    }
    pci_free_irq_vectors(pdev);
    return err;
}
EXPORT_SYMBOL_GPL(virtio_nic_setup_msix);

/* Request IRQs with legacy support */
int virtio_nic_request_irqs(struct virtio_nic_priv *priv)
{
    struct pci_dev *pdev = to_pci_dev(priv->vdev->dev.parent);
    int i, err;

    if (!priv || !pdev)
        return -EINVAL;

    /* Try MSI-X first */
    err = virtio_nic_setup_msix(priv);
    if (err == 0)
        return 0;

    /* Fallback to MSI */
    err = pci_alloc_irq_vectors(pdev, priv->num_queues, priv->num_queues,
                                PCI_IRQ_MSI);
    if (err < 0) {
        dev_err(&priv->vdev->dev, "Failed to allocate MSI vectors: %d\n", err);
        return err;
    }

    for (i = 0; i < priv->num_queues; i++) {
        priv->queues[i].irq = pci_irq_vector(pdev, i);
        err = request_irq(priv->queues[i].irq, virtio_nic_interrupt, 0,
                          "virtio_nic", &priv->queues[i]);
        if (err) {
            dev_err(&priv->vdev->dev, "Failed to request IRQ %d: %d\n", 
                    priv->queues[i].irq, err);
            break;
        }
    }

    if (err) {
        /* Cleanup on failure */
        for (i = 0; i < priv->num_queues; i++) {
            if (priv->queues[i].irq > 0) {
                free_irq(priv->queues[i].irq, &priv->queues[i]);
                priv->queues[i].irq = -1;
            }
        }
        pci_free_irq_vectors(pdev);
        return err;
    }

    dev_info(&priv->vdev->dev, "MSI setup complete with %d vectors\n", priv->num_queues);
    return 0;
}
EXPORT_SYMBOL_GPL(virtio_nic_request_irqs);

void virtio_nic_free_irqs(struct virtio_nic_priv *priv)
{
    struct pci_dev *pdev = to_pci_dev(priv->vdev->dev.parent);
    int i;

    if (!priv || !pdev)
        return;

    for (i = 0; i < priv->num_queues; i++) {
        if (priv->queues[i].irq > 0) {
            /* Remove affinity hint */
            irq_set_affinity_hint(priv->queues[i].irq, NULL);
            
            /* Free IRQ */
            free_irq(priv->queues[i].irq, &priv->queues[i]);
            priv->queues[i].irq = -1;
        }
    }

    pci_free_irq_vectors(pdev);
}
EXPORT_SYMBOL_GPL(virtio_nic_free_irqs);

/* Update interrupt coalescing based on load */
void virtio_nic_update_coalesce(int usecs)
{
    int i;
    struct virtio_nic_priv *priv = NULL;

    /* Find the first available priv structure */
    for (i = 0; i < 10; i++) { /* Arbitrary limit */
        struct net_device *ndev = dev_get_by_name(&init_net, "virtio_nic");
        if (ndev) {
            priv = netdev_priv(ndev);
            dev_put(ndev);
            break;
        }
    }

    if (!priv)
        return;

    /* Clamp coalescing time */
    usecs = max(min_coalesce_usecs, min(usecs, max_coalesce_usecs));
    coalesce_usecs = usecs;

    /* Update all queue timers */
    for (i = 0; i < priv->num_queues; i++) {
        if (priv->queues[i].coalesce_timer.function) {
            mod_timer(&priv->queues[i].coalesce_timer, 
                     jiffies + usecs_to_jiffies(usecs));
        }
    }
}
EXPORT_SYMBOL_GPL(virtio_nic_update_coalesce);

/* Adaptive interrupt coalescing based on queue load */
void virtio_nic_adaptive_coalescing(struct virtio_nic_priv *priv)
{
    int i, total_load = 0;
    int new_coalesce = coalesce_usecs;

    if (!adaptive_coalesce || !priv)
        return;

    /* Calculate total load across all queues */
    for (i = 0; i < priv->num_queues; i++) {
        total_load += atomic_read(&priv->queues[i].pending_packets);
    }

    /* Adjust coalescing based on load */
    if (total_load > 1000) {
        /* High load - reduce coalescing for lower latency */
        new_coalesce = max(min_coalesce_usecs, coalesce_usecs / 2);
    } else if (total_load < 100) {
        /* Low load - increase coalescing for efficiency */
        new_coalesce = min(max_coalesce_usecs, coalesce_usecs * 2);
    }

    if (new_coalesce != coalesce_usecs) {
        virtio_nic_update_coalesce(new_coalesce);
    }
}
EXPORT_SYMBOL_GPL(virtio_nic_adaptive_coalescing);

/* Get interrupt statistics */
void virtio_nic_get_irq_stats(struct virtio_nic_priv *priv, struct virtio_nic_irq_stats *stats)
{
    int i;

    if (!priv || !stats)
        return;

    memset(stats, 0, sizeof(*stats));
    
    for (i = 0; i < priv->num_queues; i++) {
        stats->total_irqs++;
        stats->total_packets += atomic_read(&priv->queues[i].pending_packets);
        
        if (priv->queues[i].irq > 0)
            stats->active_vectors++;
    }
    
    stats->coalesce_usecs = coalesce_usecs;
    stats->adaptive_enabled = adaptive_coalesce;
}
EXPORT_SYMBOL_GPL(virtio_nic_get_irq_stats);

/* Module initialization */
static int __init virtio_nic_irq_init(void)
{
    return 0;
}

static void __exit virtio_nic_irq_exit(void)
{
    /* Cleanup handled by main driver */
}

module_init(virtio_nic_irq_init);
module_exit(virtio_nic_irq_exit);

MODULE_DESCRIPTION("MSI-X interrupt management for VirtIO NIC driver");
MODULE_LICENSE("GPL");
