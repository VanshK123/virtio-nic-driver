// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include "virtio_nic.h"

struct dma_buffer {
    struct page    **pages;
    struct scatterlist *sgl;
    unsigned int     nents;
    unsigned int     nr_pages;
};

static int __dma_pin_pages(struct dma_buffer *buf, unsigned long start,
                           size_t len, bool write)
{
    int i, ret;
    buf->nr_pages = DIV_ROUND_UP(len, PAGE_SIZE);
    buf->pages = kcalloc(buf->nr_pages, sizeof(struct page *), GFP_KERNEL);
    if (!buf->pages)
        return -ENOMEM;

    ret = pin_user_pages_fast(start, buf->nr_pages,
                              write ? FOLL_WRITE : 0, buf->pages);
    if (ret < 0)
        return ret;

    buf->sgl = kcalloc(buf->nr_pages, sizeof(struct scatterlist), GFP_KERNEL);
    if (!buf->sgl)
        return -ENOMEM;

    sg_init_table(buf->sgl, buf->nr_pages);
    for (i = 0; i < buf->nr_pages; i++)
        sg_set_page(&buf->sgl[i], buf->pages[i], PAGE_SIZE, 0);
    buf->nents = buf->nr_pages;
    return 0;
}

int dma_alloc_buffer(struct dma_buffer *buf, unsigned long start,
                     size_t len, bool write)
{
    int ret = __dma_pin_pages(buf, start, len, write);
    if (ret)
        return ret;

    dma_map_sg(NULL, buf->sgl, buf->nents,
               write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
    return 0;
}
EXPORT_SYMBOL_GPL(dma_alloc_buffer);

void dma_free_buffer(struct dma_buffer *buf, bool write)
{
    int i;

    dma_unmap_sg(NULL, buf->sgl, buf->nents,
                 write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
    for (i = 0; i < buf->nr_pages; i++)
        unpin_user_page(buf->pages[i]);
    kfree(buf->pages);
    kfree(buf->sgl);
}
EXPORT_SYMBOL_GPL(dma_free_buffer);
