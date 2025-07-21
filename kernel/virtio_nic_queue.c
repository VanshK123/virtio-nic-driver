// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/virtio_config.h>
#include "virtio_nic.h"

int virtio_nic_setup_queues(struct virtio_nic_priv *priv)
{
    struct virtqueue *vqs[2];
    static const char * const names[] = { "rx", "tx" };
    int err;

    priv->num_queues = 2;
    priv->queues = kcalloc(priv->num_queues,
                           sizeof(struct virtio_nic_queue), GFP_KERNEL);
    if (!priv->queues)
        return -ENOMEM;

    err = virtio_find_vqs(priv->vdev, priv->num_queues, vqs,
                          NULL, names, NULL);
    if (err) {
        kfree(priv->queues);
        return err;
    }

    priv->queues[0].vq = vqs[0];
    priv->queues[1].vq = vqs[1];
    return 0;
}
EXPORT_SYMBOL_GPL(virtio_nic_setup_queues);

void virtio_nic_teardown_queues(struct virtio_nic_priv *priv)
{
    if (!priv->queues)
        return;
    priv->vdev->config->del_vqs(priv->vdev);
    kfree(priv->queues);
    priv->queues = NULL;
    priv->num_queues = 0;
}
EXPORT_SYMBOL_GPL(virtio_nic_teardown_queues);

int virtio_nic_enqueue(struct virtio_nic_queue *q, struct scatterlist *sg,
                       unsigned int out, unsigned int in, void *data)
{
    unsigned long flags;
    int err;

    spin_lock_irqsave(&q->lock, flags);
    err = virtqueue_add_sgs(q->vq, sg, out, in, data, GFP_ATOMIC);
    if (!err)
        virtqueue_kick(q->vq);
    spin_unlock_irqrestore(&q->lock, flags);
    return err;
}
EXPORT_SYMBOL_GPL(virtio_nic_enqueue);

void *virtio_nic_dequeue(struct virtio_nic_queue *q, unsigned int *len)
{
    unsigned long flags;
    void *buf;

    spin_lock_irqsave(&q->lock, flags);
    buf = virtqueue_get_buf(q->vq, len);
    spin_unlock_irqrestore(&q->lock, flags);
    if (buf)
        telemetry_record_rx();
    return buf;
}
EXPORT_SYMBOL_GPL(virtio_nic_dequeue);
