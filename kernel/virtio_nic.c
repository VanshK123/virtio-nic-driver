// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/virtio_config.h>
#include "virtio_nic.h"

static const struct net_device_ops virtio_nic_netdev_ops = {
    .ndo_open       = virtio_nic_open,
    .ndo_stop       = virtio_nic_stop,
    .ndo_start_xmit = virtio_nic_start_xmit,
};

static int virtio_nic_probe(struct virtio_device *vdev)
{
    struct net_device *ndev;
    struct virtio_nic_priv *priv;
    struct virtqueue *vqs[2];
    static const char * const names[] = { "rx", "tx" };
    int err;

    ndev = alloc_etherdev(sizeof(*priv));
    if (!ndev)
        return -ENOMEM;

    priv = netdev_priv(ndev);
    memset(priv, 0, sizeof(*priv));
    priv->vdev = vdev;
    priv->netdev = ndev;
    ndev->netdev_ops = &virtio_nic_netdev_ops;
    SET_NETDEV_DEV(ndev, &vdev->dev);
    vdev->priv = priv;

    priv->num_queues = 2;
    priv->queues = kcalloc(priv->num_queues,
                           sizeof(struct virtio_nic_queue), GFP_KERNEL);
    if (!priv->queues) {
        err = -ENOMEM;
        goto free_netdev;
    }

    err = virtio_find_vqs(vdev, priv->num_queues, vqs, NULL, names, NULL);
    if (err)
        goto free_queues;

    priv->queues[0].vq = vqs[0];
    priv->queues[1].vq = vqs[1];

    err = register_netdev(ndev);
    if (err)
        goto del_vqs;

    virtio_device_ready(vdev);
    return 0;

del_vqs:
    vdev->config->del_vqs(vdev);
free_queues:
    kfree(priv->queues);
free_netdev:
    free_netdev(ndev);
    return err;
}

static void virtio_nic_remove(struct virtio_device *vdev)
{
    struct virtio_nic_priv *priv = vdev->priv;

    if (!priv)
        return;

    unregister_netdev(priv->netdev);
    vdev->config->del_vqs(vdev);
    kfree(priv->queues);
    free_netdev(priv->netdev);
}

int virtio_nic_open(struct net_device *ndev)
{
    telemetry_init(ndev);
    netif_start_queue(ndev);
    return 0;
}

int virtio_nic_stop(struct net_device *ndev)
{
    netif_stop_queue(ndev);
    telemetry_exit();
    return 0;
}

netdev_tx_t virtio_nic_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
    struct virtio_nic_priv *priv = netdev_priv(ndev);
    struct virtqueue *vq = priv->queues[1].vq;

    virtqueue_add_outbuf(vq, &skb->data, 1, skb, GFP_ATOMIC);
    virtqueue_kick(vq);
    telemetry_record_tx();
    dev_kfree_skb_any(skb);
    return NETDEV_TX_OK;
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

MODULE_DESCRIPTION("Next-gen VirtIO NIC driver");
MODULE_LICENSE("GPL");
