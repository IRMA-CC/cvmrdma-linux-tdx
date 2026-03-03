#include "public_alloc.h"

#include <linux/swiotlb.h>

#include <linux/dma-buf.h>
#include <linux/dma-resv.h>

struct pubmem_priv {
    struct device * dma_dev;
    void * cpu_addr;
    dma_addr_t dma_addr;
};

struct pubmem_attach_priv {
    struct sg_table table;
};

static int pubmem_attach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attachment)
{
	struct pubmem_priv *priv = dmabuf->priv;
    struct pubmem_attach_priv * a;
	struct sg_table * attachment_table;
    struct scatterlist * sgl;
    int ret;

    if(dmabuf->size >= 0xffffffff)
    {
        printk(KERN_ERR "pubmem: size %lu too large\n", dmabuf->size);
        ret = -EINVAL;
        goto ret;
    }

    if(attachment->dev != priv->dma_dev)
    {
        ret = -EINVAL;
        goto ret;
    }

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
    {
		ret = -ENOMEM;
        goto ret;
    }

    attachment_table = &a->table;
    ret = sg_alloc_table(attachment_table, 1, GFP_KERNEL);
    if(ret)
    {
        goto free_a;
    }

    /* HACKY: normally illegal to do virt_to_page on alloc_coherent!
       And hence, sg_set_buf also illegal.
       But here we know that the allocation ended up in the SWIOTLB
       with swiotlb_alloc ... */

    sgl = attachment_table->sgl;
    sg_set_buf(sgl, priv->cpu_addr, dmabuf->size);
    sg_dma_len(sgl) = dmabuf->size;
    sg_dma_address(sgl) = priv->dma_addr;

    attachment->priv = a;

	return 0;

// free_table:
//     sg_free_table(attachment_table);
free_a:
    kfree(a);
ret:
    return ret;
}

static void pubmem_detach(struct dma_buf *dmabuf,
			       struct dma_buf_attachment *attachment)
{
    struct pubmem_attach_priv * a = attachment->priv;

	sg_free_table(&a->table);
	kfree(a);
}

static struct sg_table *pubmem_map_dma_buf(struct dma_buf_attachment *attachment,
						enum dma_data_direction direction)
{
    struct pubmem_attach_priv * a = attachment->priv;

	return &a->table;
}

static void pubmem_unmap_dma_buf(struct dma_buf_attachment *attachment,
				      struct sg_table *table,
				      enum dma_data_direction direction)
{
    return;
}

static int pubmem_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
						enum dma_data_direction direction)
{
    return 0;
}

static int pubmem_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
					      enum dma_data_direction direction)
{
    return 0;
}

static int pubmem_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct pubmem_priv *priv = dmabuf->priv;

	dma_resv_assert_held(dmabuf->resv);

	return dma_mmap_coherent(priv->dma_dev, vma, priv->cpu_addr, priv->dma_addr, dmabuf->size);
}

static void pubmem_dma_buf_release(struct dma_buf *dmabuf)
{
	struct pubmem_priv *priv = dmabuf->priv;

	if(priv && priv->cpu_addr)
	    dma_free_coherent(priv->dma_dev, dmabuf->size, priv->cpu_addr, priv->dma_addr);

	kfree(priv);
}


static struct dma_buf_ops pubmem_dmabuf_ops = {
	.attach = pubmem_attach,
	.detach = pubmem_detach,
	.map_dma_buf = pubmem_map_dma_buf,
	.unmap_dma_buf = pubmem_unmap_dma_buf,
	.begin_cpu_access = pubmem_dma_buf_begin_cpu_access,
	.end_cpu_access = pubmem_dma_buf_end_cpu_access,
	.mmap = pubmem_mmap,
    .release = pubmem_dma_buf_release
};

int pubmem_dmabuf_alloc(struct device * dma_dev, unsigned long len, unsigned long fd_flags)
{
    DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
    struct dma_buf * dmabuf;
    struct pubmem_priv * priv;
    void * va;
    dma_addr_t dma_addr;
    int ret;

    priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if(! priv)
    {
        ret = - ENOMEM;
        goto ret;
    }

    va = dma_alloc_coherent(dma_dev, len, &dma_addr, GFP_HIGHUSER);
    if(IS_ERR(va))
    {
        ret = - PTR_ERR(va);
        goto free_priv;
    }

    priv->dma_dev = dma_dev;
    priv->cpu_addr = va;
    priv->dma_addr = dma_addr;

    dev_dbg(dma_dev, "Allocated %lu bytes in SWIOTLB\n", len);


	exp_info.ops = &pubmem_dmabuf_ops;
	exp_info.size = len;
	exp_info.flags = fd_flags;
	exp_info.priv = priv;
    dmabuf = dma_buf_export(&exp_info);
    if(IS_ERR(dmabuf)) {
        ret = - PTR_ERR(dmabuf);
        goto free_buf;
    }

    return dma_buf_fd(dmabuf, fd_flags);

free_buf:
    dma_free_coherent(dma_dev, len, va, dma_addr);
free_priv:
    kfree(priv);
ret:
    return ret;
}
