#include <linux/module.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/skbuff.h>
#include <linux/vmalloc.h>
#include <linux/numa.h>
#include "virtio_nic.h"

/* DMA buffer pool for zero-copy operations */
struct dma_buffer_pool {
    struct virtio_nic_dma_buf *buffers;
    unsigned int size;
    unsigned int used;
    spinlock_t lock;
    int numa_node;
};

static struct dma_buffer_pool *dma_pools[NR_NUMA_NODES];

/* Allocate DMA buffer with NUMA awareness */
int virtio_nic_dma_alloc_buffer(struct virtio_nic_dma_buf *buf, size_t size, bool write)
{
    int numa_node = numa_node_id();
    struct page **pages;
    struct scatterlist *sgl;
    unsigned int nr_pages, i;
    dma_addr_t dma_addr;
    int ret;

    if (!buf)
        return -EINVAL;

    memset(buf, 0, sizeof(*buf));
    buf->size = size;
    buf->write = write;

    /* Calculate number of pages needed */
    nr_pages = DIV_ROUND_UP(size, PAGE_SIZE);
    buf->nr_pages = nr_pages;

    /* Allocate page array */
    pages = kcalloc(nr_pages, sizeof(struct page *), GFP_KERNEL);
    if (!pages)
        return -ENOMEM;
    buf->pages = pages;

    /* Allocate scatterlist */
    sgl = kcalloc(nr_pages, sizeof(struct scatterlist), GFP_KERNEL);
    if (!sgl) {
        kfree(pages);
        return -ENOMEM;
    }
    buf->sgl = sgl;

    /* Allocate pages with NUMA awareness */
    for (i = 0; i < nr_pages; i++) {
        pages[i] = alloc_pages_node(numa_node, GFP_KERNEL, 0);
        if (!pages[i]) {
            ret = -ENOMEM;
            goto free_pages;
        }
    }

    /* Initialize scatterlist */
    sg_init_table(sgl, nr_pages);
    for (i = 0; i < nr_pages; i++) {
        sg_set_page(&sgl[i], pages[i], PAGE_SIZE, 0);
    }
    buf->nents = nr_pages;

    /* Map for DMA */
    dma_addr = dma_map_sg(NULL, sgl, nr_pages, 
                          write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
    if (dma_mapping_error(NULL, dma_addr)) {
        ret = -ENOMEM;
        goto free_pages;
    }
    buf->dma_addr = dma_addr;

    return 0;

free_pages:
    for (i = 0; i < nr_pages; i++) {
        if (pages[i])
            __free_pages(pages[i], 0);
    }
    kfree(sgl);
    kfree(pages);
    return ret;
}
EXPORT_SYMBOL_GPL(virtio_nic_dma_alloc_buffer);

/* Free DMA buffer */
void virtio_nic_dma_free_buffer(struct virtio_nic_dma_buf *buf)
{
    unsigned int i;

    if (!buf)
        return;

    /* Unmap from DMA */
    if (buf->sgl && buf->nents)
        dma_unmap_sg(NULL, buf->sgl, buf->nents,
                     buf->write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);

    /* Free pages */
    if (buf->pages) {
        for (i = 0; i < buf->nr_pages; i++) {
            if (buf->pages[i])
                __free_pages(buf->pages[i], 0);
        }
        kfree(buf->pages);
    }

    /* Free scatterlist */
    if (buf->sgl)
        kfree(buf->sgl);

    memset(buf, 0, sizeof(*buf));
}
EXPORT_SYMBOL_GPL(virtio_nic_dma_free_buffer);

/* Map sk_buff for zero-copy DMA */
int virtio_nic_dma_map_skb(struct sk_buff *skb, struct scatterlist *sg, int *nents)
{
    struct skb_shared_info *shinfo = skb_shinfo(skb);
    unsigned int i, nr_frags = shinfo->nr_frags;
    int ret;

    if (!skb || !sg || !nents)
        return -EINVAL;

    /* Handle linear part */
    sg_init_table(sg, nr_frags + 1);
    sg_set_buf(&sg[0], skb->data, skb_headlen(skb));

    /* Handle fragments */
    for (i = 0; i < nr_frags; i++) {
        skb_frag_t *frag = &shinfo->frags[i];
        sg_set_page(&sg[i + 1], skb_frag_page(frag),
                   skb_frag_size(frag), skb_frag_off(frag));
    }

    *nents = nr_frags + 1;

    /* Map for DMA */
    ret = dma_map_sg(NULL, sg, *nents, DMA_TO_DEVICE);
    if (ret == 0) {
        *nents = 0;
        return -ENOMEM;
    }

    return 0;
}
EXPORT_SYMBOL_GPL(virtio_nic_dma_map_skb);

/* Initialize DMA buffer pools */
int virtio_nic_dma_init_pools(void)
{
    int i, numa_nodes = num_possible_nodes();

    for (i = 0; i < numa_nodes; i++) {
        struct dma_buffer_pool *pool;

        pool = kzalloc(sizeof(*pool), GFP_KERNEL);
        if (!pool)
            goto cleanup_pools;

        pool->size = 64; /* Pre-allocate 64 buffers per NUMA node */
        pool->buffers = kcalloc(pool->size, sizeof(struct virtio_nic_dma_buf), GFP_KERNEL);
        if (!pool->buffers) {
            kfree(pool);
            goto cleanup_pools;
        }

        spin_lock_init(&pool->lock);
        pool->numa_node = i;
        dma_pools[i] = pool;
    }

    return 0;

cleanup_pools:
    for (i = 0; i < numa_nodes; i++) {
        if (dma_pools[i]) {
            kfree(dma_pools[i]->buffers);
            kfree(dma_pools[i]);
            dma_pools[i] = NULL;
        }
    }
    return -ENOMEM;
}
EXPORT_SYMBOL_GPL(virtio_nic_dma_init_pools);

/* Cleanup DMA buffer pools */
void virtio_nic_dma_cleanup_pools(void)
{
    int i, numa_nodes = num_possible_nodes();

    for (i = 0; i < numa_nodes; i++) {
        if (dma_pools[i]) {
            unsigned int j;
            for (j = 0; j < dma_pools[i]->size; j++) {
                virtio_nic_dma_free_buffer(&dma_pools[i]->buffers[j]);
            }
            kfree(dma_pools[i]->buffers);
            kfree(dma_pools[i]);
            dma_pools[i] = NULL;
        }
    }
}
EXPORT_SYMBOL_GPL(virtio_nic_dma_cleanup_pools);

/* Get DMA buffer from pool */
struct virtio_nic_dma_buf *virtio_nic_dma_get_buffer(int numa_node, size_t size, bool write)
{
    struct dma_buffer_pool *pool;
    struct virtio_nic_dma_buf *buf = NULL;
    unsigned long flags;

    if (numa_node >= num_possible_nodes())
        numa_node = numa_node_id();

    pool = dma_pools[numa_node];
    if (!pool)
        return NULL;

    spin_lock_irqsave(&pool->lock, flags);
    
    /* Find available buffer */
    for (int i = 0; i < pool->size; i++) {
        if (pool->buffers[i].size == 0) {
            buf = &pool->buffers[i];
            break;
        }
    }

    if (buf) {
        int ret = virtio_nic_dma_alloc_buffer(buf, size, write);
        if (ret == 0) {
            pool->used++;
        } else {
            buf = NULL;
        }
    }

    spin_unlock_irqrestore(&pool->lock, flags);
    return buf;
}
EXPORT_SYMBOL_GPL(virtio_nic_dma_get_buffer);

/* Return DMA buffer to pool */
void virtio_nic_dma_put_buffer(struct virtio_nic_dma_buf *buf, int numa_node)
{
    struct dma_buffer_pool *pool;
    unsigned long flags;

    if (!buf || numa_node >= num_possible_nodes())
        return;

    pool = dma_pools[numa_node];
    if (!pool)
        return;

    spin_lock_irqsave(&pool->lock, flags);
    
    /* Find and free buffer */
    for (int i = 0; i < pool->size; i++) {
        if (&pool->buffers[i] == buf) {
            virtio_nic_dma_free_buffer(buf);
            pool->used--;
            break;
        }
    }

    spin_unlock_irqrestore(&pool->lock, flags);
}
EXPORT_SYMBOL_GPL(virtio_nic_dma_put_buffer);

/* Optimized scatter-gather list creation for large packets */
int virtio_nic_create_sgl(struct scatterlist *sg, void *data, size_t len, int max_sg)
{
    unsigned int offset = 0;
    int sg_count = 0;
    size_t chunk_size;

    if (!sg || !data || !len || max_sg <= 0)
        return -EINVAL;

    sg_init_table(sg, max_sg);

    while (len > 0 && sg_count < max_sg) {
        chunk_size = min_t(size_t, len, VIRTIO_NIC_DMA_CHUNK_SIZE);
        
        sg_set_buf(&sg[sg_count], data + offset, chunk_size);
        
        offset += chunk_size;
        len -= chunk_size;
        sg_count++;
    }

    return sg_count;
}
EXPORT_SYMBOL_GPL(virtio_nic_create_sgl);

/* Module initialization and cleanup */
static int __init virtio_nic_dma_init(void)
{
    return virtio_nic_dma_init_pools();
}

static void __exit virtio_nic_dma_exit(void)
{
    virtio_nic_dma_cleanup_pools();
}

module_init(virtio_nic_dma_init);
module_exit(virtio_nic_dma_exit);

MODULE_DESCRIPTION("Zero-copy DMA support for VirtIO NIC driver");
MODULE_LICENSE("GPL");
