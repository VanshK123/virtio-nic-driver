// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include "virtio_nic.h"

static irqreturn_t virtio_nic_interrupt(int irq, void *data)
{
    struct virtio_nic_queue *q = data;

    if (virtqueue_disable_cb(q->vq))
        napi_schedule(&q->napi);

    return IRQ_HANDLED;
}

int virtio_nic_request_irqs(struct virtio_nic_priv *priv)
{
    struct pci_dev *pdev = to_pci_dev(priv->vdev->dev.parent);
    int i, err;

    err = pci_alloc_irq_vectors(pdev, priv->num_queues, priv->num_queues,
                                PCI_IRQ_MSIX);
    if (err < 0)
        return err;

    for (i = 0; i < priv->num_queues; i++) {
        priv->queues[i].irq = pci_irq_vector(pdev, i);
        err = request_irq(priv->queues[i].irq, virtio_nic_interrupt, 0,
                          "virtio_nic", &priv->queues[i]);
        if (err)
            break;
    }

    return err;
}
EXPORT_SYMBOL_GPL(virtio_nic_request_irqs);

void virtio_nic_free_irqs(struct virtio_nic_priv *priv)
{
    struct pci_dev *pdev = to_pci_dev(priv->vdev->dev.parent);
    int i;

    for (i = 0; i < priv->num_queues; i++)
        free_irq(priv->queues[i].irq, &priv->queues[i]);

    pci_free_irq_vectors(pdev);
}
EXPORT_SYMBOL_GPL(virtio_nic_free_irqs);

/* Simple adaptive interrupt coalescing placeholder */
static int coalesce_usecs = 64;
module_param(coalesce_usecs, int, 0644);
MODULE_PARM_DESC(coalesce_usecs, "Interrupt coalescing time in usecs");

void virtio_nic_update_coalesce(int usecs)
{
    coalesce_usecs = usecs;
}
EXPORT_SYMBOL_GPL(virtio_nic_update_coalesce);
