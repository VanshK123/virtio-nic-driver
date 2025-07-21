#include <kunit/test.h>
#include <linux/scatterlist.h>
#include <linux/mm.h>
#include <linux/slab.h>

static void dma_alignment_test(struct kunit *test)
{
    const int nents = 4;
    struct scatterlist *sgl;
    int i;

    sgl = kcalloc(nents, sizeof(*sgl), GFP_KERNEL);
    KUNIT_ASSERT_NOT_NULL(test, sgl);

    for (i = 0; i < nents; i++) {
        void *buf = (void *)__get_free_pages(GFP_KERNEL, 0);
        KUNIT_ASSERT_NOT_NULL(test, buf);
        sg_init_one(&sgl[i], buf, PAGE_SIZE);
    }

    for (i = 0; i < nents; i++)
        KUNIT_EXPECT_TRUE(test,
            IS_ALIGNED((unsigned long)sg_virt(&sgl[i]), PAGE_SIZE));

    for (i = 0; i < nents; i++)
        free_pages((unsigned long)sg_virt(&sgl[i]), 0);

    kfree(sgl);
}

static struct kunit_case dma_test_cases[] = {
    KUNIT_CASE(dma_alignment_test),
    {}
};

static struct kunit_suite dma_test_suite = {
    .name = "virtio_nic_dma_alignment",
    .test_cases = dma_test_cases,
};

kunit_test_suite(dma_test_suite);

MODULE_LICENSE("GPL");
