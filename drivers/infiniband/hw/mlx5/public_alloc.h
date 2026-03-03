#pragma once

#include <linux/types.h>
#include <linux/device.h>

int pubmem_dmabuf_alloc(struct device * dma_dev, unsigned long len, unsigned long fd_flags);
