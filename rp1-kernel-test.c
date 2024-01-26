#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/cdev.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

#include "rp1-kernel-test.h"

// not sure how to go from example_open back to the platform_device and example_state
static struct example_state *gs;
static dev_t characterDevice;

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

static int example_open(struct inode *inode, struct file *file) {
  file->private_data = gs;
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
  int ret = len;
  struct dma_async_tx_descriptor *desc;
  dma_addr_t dma;
  struct device * dev = state->rx_chan->device->dev;

  struct dma_packet_in_progress *packet_state = devm_kmalloc(state->dev, sizeof(struct dma_packet_in_progress), GFP_KERNEL);
  if (!packet_state) {
    return -ENOMEM;
  }
  init_waitqueue_head(&packet_state->wait_queue);
  packet_state->dma_done = false;

  char *buffer = dma_alloc_noncoherent(dev, len, &dma, DMA_FROM_DEVICE, GFP_KERNEL);
  if (!buffer) return -ENOMEM;
  desc = dmaengine_prep_slave_single(state->rx_chan, dma, len, DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
  desc->callback_result = dma_complete2;
  desc->callback_param = packet_state;

  int cookie = dmaengine_submit(desc);
  int retr = dma_submit_error(cookie);
  //printk(KERN_INFO"ret %d, cookie %d\n", retr, cookie);
  dma_async_issue_pending(state->rx_chan);

  // block until dma complete
  wait_event(packet_state->wait_queue, packet_state->dma_done);

  dma_sync_single_for_device(dev, dma, len, DMA_FROM_DEVICE);
#if 1
  if (copy_to_user(data, buffer, len) != 0) {
    ret = -EFAULT;
    goto done;
  }
#endif
done:
  dma_free_noncoherent(dev, len, buffer, dma, DMA_TO_DEVICE);
  devm_kfree(state->dev, packet_state);
  return len;
}

static int example_release(struct inode *inode, struct file *file) {
  file->private_data = NULL;
  return 0;
}

static int example_probe_rx(struct platform_device *pdev) {
  struct dma_slave_config rx_conf = {
    // RP1 dma driver only uses addr_width for the device end
    .dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
    .src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
    .direction = DMA_DEV_TO_MEM,
    .dst_maxburst = 4,
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

module_platform_driver(example_driver);

MODULE_LICENSE("GPL");

