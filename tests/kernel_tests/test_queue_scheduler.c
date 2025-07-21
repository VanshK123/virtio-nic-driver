#include <kunit/test.h>
#include <linux/slab.h>

struct virtqueue_binding {
    int cpu;
};

static void assign_queues_round_robin(struct virtqueue_binding *q, int n, int cpus)
{
    int i;
    for (i = 0; i < n; i++)
        q[i].cpu = i % cpus;
}

static void queue_scheduler_test(struct kunit *test)
{
    const int num_queues = 4;
    const int num_cpus = 4;
    struct virtqueue_binding *qs;
    int i;

    qs = kcalloc(num_queues, sizeof(*qs), GFP_KERNEL);
    KUNIT_ASSERT_NOT_NULL(test, qs);

    assign_queues_round_robin(qs, num_queues, num_cpus);

    for (i = 0; i < num_queues; i++)
        KUNIT_EXPECT_EQ(test, qs[i].cpu, i % num_cpus);

    kfree(qs);
}

static struct kunit_case scheduler_cases[] = {
    KUNIT_CASE(queue_scheduler_test),
    {}
};

static struct kunit_suite scheduler_suite = {
    .name = "virtio_nic_queue_scheduler",
    .test_cases = scheduler_cases,
};

kunit_test_suite(scheduler_suite);

MODULE_LICENSE("GPL");
