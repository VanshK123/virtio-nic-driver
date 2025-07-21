/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VIRTIO_NIC_H
#define VIRTIO_NIC_H

#include <linux/netdevice.h>
#include <linux/virtio.h>
#include <linux/virtio_net.h>
#include <linux/pci.h>

struct virtio_nic_queue {
    struct virtqueue *vq;
    struct napi_struct napi;
    spinlock_t lock;
    u32 flow_tag;
    int irq;
};

struct virtio_nic_priv {
    struct virtio_device *vdev;
    struct net_device *netdev;
    struct virtio_nic_queue *queues;
    unsigned int num_queues;
};

int virtio_nic_init(void);
void virtio_nic_exit(void);
int virtio_nic_open(struct net_device *ndev);
int virtio_nic_stop(struct net_device *ndev);
netdev_tx_t virtio_nic_start_xmit(struct sk_buff *skb, struct net_device *ndev);

void telemetry_init(struct net_device *ndev);
void telemetry_exit(void);
void telemetry_record_tx(void);
void telemetry_record_rx(void);

#endif /* VIRTIO_NIC_H */
