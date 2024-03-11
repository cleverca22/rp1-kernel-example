#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/cdev.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

#include "rp1-kernel-test.h"

static int ringbuffer_size = 1024 * 1024 * 16;
module_param(ringbuffer_size, int, 0444);

// not sure how to go from example_open back to the platform_device and example_state
static struct example_state *gs;
static dev_t characterDevice;
static struct class *pio_class;

static int example_open(struct inode *inode, struct file *file);
static ssize_t example_write(struct file *file, const char *data, size_t len,  loff_t *offset);
static ssize_t example_read(struct file *file, char *data, size_t len,  loff_t *offset);
static int example_release(struct inode *inode, struct file *file);

static const struct of_device_id example_ids[] = {
  { .compatible = "rp1,example", },
  { .compatible = "rp1,rx-example", },
  {}
};
MODULE_DEVICE_TABLE(of, example_ids);

static struct file_operations char_fops_rx = {
  .owner = THIS_MODULE,
  .open = example_open,
  .read = example_read,
  .release = example_release,
};

static struct file_operations char_fops_tx = {
  .owner = THIS_MODULE,
  .open = example_open,
  .write = example_write,
  .release = example_release,
};

static void dma_cycle_complete(void *ptr, const struct dmaengine_result *result) {
  //enum dma_status dmastat;
  //struct dma_tx_state dma_state;
  struct example_state *state = ptr;

  printk(KERN_INFO"dma complete3 %d %d\n", result->result, result->residue);

  //dmastat = dmaengine_tx_status(state->rx_chan, state->rx_ring_cookie, &dma_state);
  // the dma_state.residue is how many bytes remain to be copied for the current cycle
  // because dmaengine_prep_dma_cyclic was set with len/2, this callback gets ran twice, when the write-pointer is at the start and middle of the buffer
  // due to latencies in the irq, the write-ptr is already to 1022kb past the expected point
  //printk(KERN_INFO"last:%d used:%d residue:%d in_flight_bytes:%d, mycookie:%d\n", dma_state.last, dma_state.used, dma_state.residue, dma_state.in_flight_bytes, state->rx_ring_cookie);

  state->chunk_received = true;
  wake_up(&state->wait_queue);

#if 0
  if (ptr) {
    struct dma_packet_in_progress *ps = ptr;
    ps->dma_done = true;
    wake_up(&ps->wait_queue);
  }
#endif
}

static int start_dma_rx_ring(struct example_state *state, int len) {
  struct dma_async_tx_descriptor *desc;
  struct device * dev = state->rx_chan->device->dev;

  state->buffer = dma_alloc_noncoherent(dev, len, &state->dma, DMA_FROM_DEVICE, GFP_KERNEL);
  if (!state->buffer) return -ENOMEM;

  // completion callback gets ran after every len/2 bytes
  desc = dmaengine_prep_dma_cyclic(state->rx_chan, state->dma, len, len/2, DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
  if (!desc) {
    dev_err(state->dev, "Preparing DMA cyclic failed\n");
    return -ENOMEM;
  }
  desc->callback_result = dma_cycle_complete;
  desc->callback_param = state;

  int cookie = dmaengine_submit(desc);
  int retr = dma_submit_error(cookie);
  //printk(KERN_INFO"ret %d, cookie %d\n", retr, cookie);
  dma_async_issue_pending(state->rx_chan);

  state->desc = desc;
  state->rx_ring_cookie = cookie;

  return 0;
}

static int example_open(struct inode *inode, struct file *file) {
  file->private_data = gs;

  // TODO, grab a lock
  if (gs->open_handle) return -EBUSY;
  gs->open_handle = file;
  gs->read_ptr = 0;

  init_waitqueue_head(&gs->wait_queue);
  start_dma_rx_ring(gs, ringbuffer_size);
  return 0;
}

static void dma_complete2(void *ptr, const struct dmaengine_result *result) {
  //printk(KERN_INFO"dma complete2 %d %d\n", result->result, result->residue);
  if (ptr) {
    struct dma_packet_in_progress *ps = ptr;
    ps->dma_done = true;
    wake_up(&ps->wait_queue);
  }
}

static ssize_t example_write_direct(struct file *file, const char *data, size_t len,  loff_t *offset) {
  struct example_state *state = file->private_data;
  int ret = len;
  char *buffer = devm_kmalloc(state->dev, len, GFP_KERNEL);

  if (copy_from_user(buffer, data, len) != 0) {
    ret = -EFAULT;
    goto done;
  }

  for (int i=0; i<len; i++) {
    writel(buffer[i], state->regs);
  }

done:
  devm_kfree(state->dev, buffer);
  return ret;
}

static ssize_t example_write_dma(struct file *file, const char *data, size_t len,  loff_t *offset) {
  struct example_state *state = file->private_data;
  int ret = len;
  struct dma_async_tx_descriptor *desc;
  dma_addr_t dma;
  struct device * dev = state->tx_chan->device->dev;
  char *buffer = dma_alloc_noncoherent(dev, len, &dma, DMA_TO_DEVICE, GFP_KERNEL);

  printk(KERN_INFO"write %llx %ld %llx %llx\n", (uint64_t)buffer, len, dma, (uint64_t)dev);
  if (!buffer) return -ENOMEM;

  if (copy_from_user(buffer, data, len) != 0) {
    ret = -EFAULT;
    goto done;
  }

  //dma_addr_t iommu_dma_addr = dma_map_single(state->chan->device->dev, buffer, len, DMA_TO_DEVICE);

  dma_sync_single_for_device(dev, dma, len, DMA_TO_DEVICE);

  desc = dmaengine_prep_slave_single(state->tx_chan, dma, len, DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
  desc->callback_result = dma_complete2;
  desc->callback_param = NULL;

  printk(KERN_INFO"phys: %llx\n", (uint64_t)desc->phys);
  printk(KERN_INFO"chan: %llx\n", (uint64_t)desc->chan);
  printk(KERN_INFO"tx_submit: %llx\n", (uint64_t)desc->tx_submit);
  //printk(KERN_INFO": %x\n", desc->);
  int cookie = dmaengine_submit(desc);
  int retr = dma_submit_error(cookie);
  printk(KERN_INFO"ret %d, cookie %d\n", retr, cookie);

  dma_async_issue_pending(state->tx_chan);
  printk(KERN_INFO"issued, nap time\n");

  msleep(10000);
  printk(KERN_INFO"nap done\n");
done:
  //dma_free_coherent(dev, len, buffer, dma);
  dma_free_noncoherent(dev, len, buffer, dma, DMA_TO_DEVICE);
  return ret;
}

static ssize_t example_write(struct file *file, const char *data, size_t len,  loff_t *offset) {
  if (len < 2) return example_write_direct(file, data, len, offset);
  else return example_write_dma(file, data, len, offset);
}

static ssize_t example_read(struct file *file, char *data, size_t len,  loff_t *offset) {
  //printk(KERN_INFO"example_read(%p, %p, %ld, offset)\n", file, data, len);
  struct example_state *state = file->private_data;
  int ret;
  struct device * dev = state->rx_chan->device->dev;
  enum dma_status dmastat;
  struct dma_tx_state dma_state;

  // block until dma complete
  wait_event(state->wait_queue, state->chunk_received);

  dmastat = dmaengine_tx_status(state->rx_chan, state->rx_ring_cookie, &dma_state);
  // the dma_state.residue is how many bytes remain to be copied for the current cycle
  // because dmaengine_prep_dma_cyclic was set with len/2, this callback gets ran twice, when the write-pointer is at the start and middle of the buffer
  // due to latencies in the irq, the write-ptr is already to 1022kb past the expected point
  printk(KERN_INFO"last:%d used:%d residue:%d in_flight_bytes:%d, mycookie:%d\n", dma_state.last, dma_state.used, dma_state.residue, dma_state.in_flight_bytes, state->rx_ring_cookie);

  uint64_t write_ptr = ringbuffer_size - dma_state.residue;

  printk(KERN_INFO"read buf:%llx len:%ld data:0x%llx\n", (uint64_t)state->buffer, len, (uint64_t)data);
  printk(KERN_INFO"writeptr: %lld, readptr: %lld\n", write_ptr, state->read_ptr);

  unsigned int available;
  if (state->read_ptr <= write_ptr) {
    available = write_ptr - state->read_ptr;
  } else if (state->read_ptr > write_ptr) {
    available = (write_ptr + ringbuffer_size) - state->read_ptr;
  }
  printk(KERN_INFO"available: %d\n", available);

  if (len > available) len = available;

  unsigned int tocopy = min(len, available);

  // TODO, when the write pointer wraps, this doesnt respect the length userland asked for, causing buffer overflow
  if (state->read_ptr <= write_ptr) {
    printk(KERN_INFO"simplecase %d\n", tocopy);
    dma_sync_single_for_device(dev, state->dma + state->read_ptr, tocopy, DMA_FROM_DEVICE);
    if (copy_to_user(data, state->buffer + state->read_ptr, tocopy) != 0) {
      ret = -EFAULT;
      goto done;
    }
    state->read_ptr += tocopy;
    ret = tocopy;
  } else if (state->read_ptr > write_ptr) {
    printk(KERN_INFO"complex 1\n");
    unsigned int len1 = ringbuffer_size - state->read_ptr;
    dma_sync_single_for_device(dev, state->dma + state->read_ptr, len1, DMA_FROM_DEVICE);
    printk(KERN_INFO"copy_to_user(0x%llx, 0x%llx, %ld)\n", (uint64_t)(data), (uint64_t)(state->buffer + state->read_ptr), len);
    if (copy_to_user(data, state->buffer + state->read_ptr, len) != 0) {
      ret = -EFAULT;
      goto done;
    }
    state->read_ptr += len1;
    state->read_ptr = state->read_ptr % ringbuffer_size;

    unsigned int len2 = write_ptr;
    printk(KERN_INFO"complex 2 len1:%d len2:%d\n", len1, len2);
    dma_sync_single_for_device(dev, state->dma + state->read_ptr, len2, DMA_FROM_DEVICE);
    printk(KERN_INFO"copy_to_user(0x%llx, 0x%llx, %d)\n", (uint64_t)(data + len1), (uint64_t)state->buffer, len2);
    if (copy_to_user(data + len1, state->buffer, len2) != 0) {
      ret = -EFAULT;
      goto done;
    }
    state->read_ptr += len2;
    ret = len1 + len2;
  }

  state->chunk_received = false;

done:
  printk(KERN_INFO"readptr: %lld\n", state->read_ptr);
  printk(KERN_INFO"ret %d\n", ret);
  return ret;
}

static int example_release(struct inode *inode, struct file *file) {
  struct example_state *state = file->private_data;
  dmaengine_terminate_sync(state->rx_chan);
  int len = ringbuffer_size;

  struct device * dev = state->rx_chan->device->dev;

  dma_free_noncoherent(dev, len, state->buffer, state->dma, DMA_FROM_DEVICE);

  file->private_data = NULL;
  state->open_handle = NULL;
  return 0;
}

static int example_probe_rx(struct platform_device *pdev) {
  struct dma_slave_config rx_conf = {
    // RP1 dma driver only uses addr_width for the device end
    .dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
    .src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
    .direction = DMA_DEV_TO_MEM,
    .dst_maxburst = 1,
    .src_maxburst = 1,
    .dst_port_window_size = 1,
    .device_fc = false,
  };
  struct device * dev = &pdev->dev;
  struct example_state *state;
  struct resource *mem;
  int ret = 0;

  state = (struct example_state*) devm_kmalloc(dev, sizeof(struct example_state), GFP_KERNEL);
  if (!state) {
    dev_err(dev, "Couldnt allocate state\n");
    return -ENOMEM;
  }
  state->dev = dev;
  state->open_handle = NULL;
  state->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &mem);
  rx_conf.src_addr = (uint64_t)mem->start;

  // TODO, check for null
  state->chardev = cdev_alloc();

  state->chardev->owner = THIS_MODULE;
  state->chardev->ops = &char_fops_rx;

  state->tx_chan = NULL;
  state->rx_chan = dma_request_chan(dev, "rx");
  if (IS_ERR(state->rx_chan)) {
    ret = PTR_ERR(state->rx_chan);
    printk(KERN_ERR"failed to request dma channel %d\n", ret);
    goto fail;
  }

  dmaengine_slave_config(state->rx_chan, &rx_conf);

  alloc_chrdev_region(&characterDevice,0,1,"example");
  cdev_add(state->chardev, characterDevice, 1);
  dev_set_drvdata(dev, state);
  gs = state;

  if (IS_ERR(device_create(pio_class, dev, characterDevice, NULL, "example%d", 1))) {
    dev_err(dev, "cant create device\n");
  }

  printk(KERN_INFO"example rx driver loaded\n");
  return 0;
fail:
  cdev_del(state->chardev);
  unregister_chrdev_region(characterDevice, 1);
  gs = NULL;
  devm_kfree(dev, state);
  return ret;
}

static int example_probe_tx(struct platform_device *pdev) {
  struct example_state *state;
  struct device * dev = &pdev->dev;
  struct resource *mem;
  struct dma_slave_config tx_conf = {
    .dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
    .src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
    .direction = DMA_MEM_TO_DEV,
    .dst_maxburst = 4,
    .dst_port_window_size = 1,
    .device_fc = false,
  };
  int ret = 0;

  state = (struct example_state*) devm_kmalloc(dev, sizeof(struct example_state), GFP_KERNEL);
  if (!state) {
    dev_err(dev, "Couldnt allocate state\n");
    return -ENOMEM;
  }
  state->dev = dev;

  state->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &mem);

  // must be the physical addr from the linux arm view, not a virt addr
  tx_conf.dst_addr = (uint64_t)mem->start;

  writel('U', state->regs);

  // TODO, check for null
  state->chardev = cdev_alloc();

  state->chardev->owner = THIS_MODULE;
  state->chardev->ops = &char_fops_tx;

  state->rx_chan = NULL;
  state->tx_chan = dma_request_chan(dev, "tx");
  if (IS_ERR(state->tx_chan)) {
    ret = PTR_ERR(state->tx_chan);
    printk(KERN_ERR"failed to request dma channel %d\n", ret);
    goto fail;
  }

  dmaengine_slave_config(state->tx_chan, &tx_conf);

  alloc_chrdev_region(&characterDevice,0,1,"example");

  cdev_add(state->chardev, characterDevice, 1);

  dev_set_drvdata(dev, state);
  gs = state;

  printk(KERN_INFO"example driver loaded\n");
  return 0;
fail:
  cdev_del(state->chardev);
  unregister_chrdev_region(characterDevice, 1);
  gs = NULL;
  devm_kfree(dev, state);
  return ret;
}

static int example_probe(struct platform_device *pdev) {
  struct device * dev = &pdev->dev;
  if (of_device_is_compatible(dev->of_node, "rp1,example")) {
    return example_probe_tx(pdev);
  } else if (of_device_is_compatible(dev->of_node, "rp1,rx-example")) {
    return example_probe_rx(pdev);
  }
  printk(KERN_INFO"warning, unhandled compatible\n");
  return 0;
}

static int example_remove(struct platform_device *pdev) {
  struct device * dev = &pdev->dev;
  struct example_state *state = dev_get_drvdata(dev);
  printk(KERN_INFO"example driver unloading\n");

  device_destroy(pio_class, characterDevice);
  cdev_del(state->chardev);
  unregister_chrdev_region(characterDevice, 1);

  if (state->tx_chan) {
    dmaengine_terminate_sync(state->tx_chan);
    dma_release_channel(state->tx_chan);
  }
  if (state->rx_chan) {
    dmaengine_terminate_sync(state->rx_chan);
    dma_release_channel(state->rx_chan);
  }

  gs = NULL;
  devm_kfree(dev, state);
  printk(KERN_INFO"example driver unloaded\n");
  return 0;
}

static struct platform_driver example_driver = {
  .driver = {
    .name = "rp1-example-driver",
    .owner = THIS_MODULE,
    .of_match_table = example_ids,
  },
  .probe = example_probe,
  .remove = example_remove,
};

int pio_init_module(void) {
  pio_class = class_create("pio");
  platform_driver_register(&example_driver);
  return 0;
}

void pio_remove_module(void) {
  platform_driver_unregister(&example_driver);
  class_destroy(pio_class);
}

module_init(pio_init_module);
module_exit(pio_remove_module);

MODULE_LICENSE("GPL");

