// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/netdevice.h>
#include <linux/sysfs.h>
#include "virtio_nic.h"

static struct perf_event *tx_event;
static struct perf_event *rx_event;
static struct kobject *telemetry_kobj;

static ssize_t tx_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
    u64 val = 0;
    if (tx_event)
        val = perf_event_read_value(tx_event, NULL, NULL);
    return sprintf(buf, "%llu\n", val);
}

static struct kobj_attribute tx_attr = __ATTR_RO(tx);

void telemetry_init(struct net_device *ndev)
{
    struct perf_event_attr attr = {
        .type = PERF_TYPE_SOFTWARE,
        .config = PERF_COUNT_SW_CPU_CLOCK,
    };

    tx_event = perf_event_create_kernel_counter(&attr, -1, NULL, NULL, NULL);
    rx_event = perf_event_create_kernel_counter(&attr, -1, NULL, NULL, NULL);

    telemetry_kobj = kobject_create_and_add("virtio_nic", &ndev->dev.kobj);
    if (telemetry_kobj)
        sysfs_create_file(telemetry_kobj, &tx_attr.attr);
}
EXPORT_SYMBOL_GPL(telemetry_init);

void telemetry_exit(void)
{
    if (telemetry_kobj) {
        kobject_put(telemetry_kobj);
        telemetry_kobj = NULL;
    }
    if (tx_event)
        perf_event_release_kernel(tx_event);
    if (rx_event)
        perf_event_release_kernel(rx_event);
}
EXPORT_SYMBOL_GPL(telemetry_exit);

void telemetry_record_tx(void)
{
    if (tx_event)
        perf_event_inc(tx_event);
}
EXPORT_SYMBOL_GPL(telemetry_record_tx);

void telemetry_record_rx(void)
{
    if (rx_event)
        perf_event_inc(rx_event);
}
EXPORT_SYMBOL_GPL(telemetry_record_rx);

