#!/bin/bash
set -e

virtio-nic-loader load virtio_nic.ko
ip link show virtio_nic0 >/dev/null

virtio-nic-loader unload virtio_nic
! ip link show virtio_nic0 >/dev/null
