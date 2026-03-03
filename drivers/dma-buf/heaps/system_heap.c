// SPDX-License-Identifier: GPL-2.0
/*
 * DMABUF System heap exporter
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (C) 2019, 2020 Linaro Ltd.
 *
 * Portions based off of Andrew Davis' SRAM heap:
 * Copyright (C) 2019 Texas Instruments Incorporated - http://www.ti.com/
 *	Andrew F. Davis <afd@ti.com>
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dma-heap.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

static struct dma_heap *sys_heap;

struct system_heap_buffer {
	struct dma_heap *heap;
	struct list_head attachments;
	struct mutex lock;
	unsigned long len;
	struct device *dev;
	dma_addr_t dma_addr;
	void * cpu_addr;
};

struct dma_heap_attachment {
	struct device *dev;
	struct sg_table *table;
	struct list_head list;
	bool mapped;
};

/*
 * The selection of the orders used for allocation (1MB, 64K, 4K) is designed
 * to match with the sizes often found in IOMMUs. Using order 4 pages instead
 * of order 0 pages can significantly improve the performance of many IOMMUs
 * by reducing TLB pressure and time spent updating page tables.
 */
static const unsigned int orders[] = {8, 4, 0};
#define NUM_ORDERS ARRAY_SIZE(orders)

static struct sg_table *new_sg_table(unsigned int nents)
{
	struct sg_table *new_table;
	int ret;

	new_table = kzalloc(sizeof(*new_table), GFP_KERNEL);
	if (!new_table)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(new_table, nents, GFP_KERNEL);
	if (ret) {
		kfree(new_table);
		return ERR_PTR(-ENOMEM);
	}

	return new_table;
}

static int system_heap_attach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attachment)
{
	struct system_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a;
	struct sg_table *table;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	INIT_LIST_HEAD(&a->list);
	a->mapped = false;

	attachment->priv = a;

	mutex_lock(&buffer->lock);
	buffer->dev = attachment->dev;
	list_add(&a->list, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

static void system_heap_detach(struct dma_buf *dmabuf,
			       struct dma_buf_attachment *attachment)
{
	struct system_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a = attachment->priv;

	mutex_lock(&buffer->lock);
	list_del(&a->list);
	mutex_unlock(&buffer->lock);

	sg_free_table(a->table);
	kfree(a->table);
	kfree(a);
}

static struct sg_table *system_heap_map_dma_buf(struct dma_buf_attachment *attachment,
						enum dma_data_direction direction)
{
	struct dma_buf *dmabuf = attachment->dmabuf;
	struct system_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a = attachment->priv;

	struct sg_table *table;
	void * cpu_addr;
	dma_addr_t dma_addr;
	int ret;

	mutex_lock(&buffer->lock);

	const unsigned int MAX_SIZE = 0xfffff000; // 0xffffffff - (0xffffffff%4096)
	unsigned int nents = buffer->len / MAX_SIZE;
	if(buffer->len % MAX_SIZE) {
		nents++;
	}

	table = new_sg_table(nents);
	if (IS_ERR(table)) {
		kfree(a);
		return -ENOMEM;
	}
	a->table = table;

	/* As recommended in kernel doc: do the allocation on first map! Normally by 
	   collating the requirements, but here should work for any device. */
	if(! buffer->cpu_addr)
	{
		cpu_addr = dma_alloc_coherent(attachment->dev, buffer->len, &dma_addr, GFP_HIGHUSER);
		if(IS_ERR(cpu_addr))
		{
			// printk(KERN_ERR "[KERNEL SYSTEM_HEAP] Failed to allocate memory\n");
			return cpu_addr;
		}

		buffer->cpu_addr = cpu_addr;
		buffer->dma_addr = dma_addr;
	}
	else
	{
		cpu_addr = buffer->cpu_addr;
		dma_addr = buffer->dma_addr;

	
	}
	mutex_unlock(&buffer->lock);

	// Iterate over entries in the table
	unsigned int i = 0;
	struct scatterlist *sg;
	unsigned int len;
	for_each_sgtable_sg(table, sg, i) {
		if (i == nents - 1)
			len = buffer->len % MAX_SIZE;
		else
			len = MAX_SIZE;

		sg_set_buf(sg, cpu_addr + i*MAX_SIZE, len);
		sg_dma_address(sg) = dma_addr + i*MAX_SIZE;
		sg_dma_len(sg) = len;
		i++;
	}

	a->mapped = true;
	return table;

}

static void system_heap_unmap_dma_buf(struct dma_buf_attachment *attachment,
				      struct sg_table *table,
				      enum dma_data_direction direction)
{
	struct dma_heap_attachment *a = attachment->priv;
	a->mapped = false;
}

static int system_heap_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
						enum dma_data_direction direction)
{
	return 0;
}

static int system_heap_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
					      enum dma_data_direction direction)
{
	return 0;
}

static int system_heap_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct system_heap_buffer *buffer = dmabuf->priv;
	return dma_mmap_coherent(buffer->dev, vma, buffer->cpu_addr, buffer->dma_addr, buffer->len);
}

static void system_heap_dma_buf_release(struct dma_buf *dmabuf)
{
	struct system_heap_buffer *buffer = dmabuf->priv;
	struct sg_table *table;
	struct scatterlist *sg;
	int i;

	dma_free_coherent(buffer->dev, buffer->len, buffer->cpu_addr, buffer->dma_addr);

	kfree(buffer);
}

static const struct dma_buf_ops system_heap_buf_ops = {
	.attach = system_heap_attach,
	.detach = system_heap_detach,
	.map_dma_buf = system_heap_map_dma_buf,
	.unmap_dma_buf = system_heap_unmap_dma_buf,
	.begin_cpu_access = system_heap_dma_buf_begin_cpu_access,
	.end_cpu_access = system_heap_dma_buf_end_cpu_access,
	.mmap = system_heap_mmap,
	.release = system_heap_dma_buf_release,
};

static struct dma_buf *system_heap_allocate(struct dma_heap *heap,
					    unsigned long len,
					    unsigned long fd_flags,
					    unsigned long heap_flags)
{
	struct system_heap_buffer *buffer;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	int i, ret = -ENOMEM;
	len = ALIGN(len, PAGE_SIZE);

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&buffer->attachments);
	mutex_init(&buffer->lock);
	buffer->heap = heap;
	buffer->len = len;
	buffer->dev = NULL;
	buffer->dma_addr = NULL;
	buffer->cpu_addr = NULL;


	/* create the dmabuf */
	exp_info.exp_name = dma_heap_get_name(heap);
	exp_info.ops = &system_heap_buf_ops;
	exp_info.size = buffer->len;
	exp_info.flags = fd_flags;
	exp_info.priv = buffer;
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto free_buffer;
	}
	return dmabuf;
free_buffer:
	kfree(buffer);

	return ERR_PTR(ret);
}

static const struct dma_heap_ops system_heap_ops = {
	.allocate = system_heap_allocate,
};

static int system_heap_create(void)
{
	struct dma_heap_export_info exp_info;

	exp_info.name = "system";
	exp_info.ops = &system_heap_ops;
	exp_info.priv = NULL;

	sys_heap = dma_heap_add(&exp_info);
	if (IS_ERR(sys_heap))
		return PTR_ERR(sys_heap);

	return 0;
}
module_init(system_heap_create);
