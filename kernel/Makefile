obj-y += virtio_nic.o virtio_nic_dma.o virtio_nic_queue.o virtio_nic_irq.o telemetry_hooks.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
$(MAKE) -C $(KDIR) M=$(PWD) clean

