#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/cdev.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

struct example_state {
  struct cdev *chardev;
  void *regs;
  struct device * dev;
  struct dma_chan *chan;
};

// not sure how to go from example_open back to the platform_device and example_state
static struct example_state *gs;

static const struct of_device_id example_ids[] = {
  { .compatible = "rp1,example", },
  {}
};
MODULE_DEVICE_TABLE(of, example_ids);

static dev_t characterDevice;

static int example_open(struct inode *, struct file *file) {
  file->private_data = gs;
  return 0;
}

static ssize_t example_write(struct file *file, const char *data, size_t len,  loff_t *) {
  struct example_state *state = file->private_data;
  int ret = len;
  char *buffer;

#if 1
  dma_addr_t dma;
  struct device * dev = state->chan->device->dev;
  buffer = dma_alloc_noncoherent(dev, len, &dma, DMA_TO_DEVICE, GFP_KERNEL);

  printk(KERN_INFO"write %llx %ld %llx %llx\n", buffer, len, dma, dev);
  if (!buffer) return -ENOMEM;

  if (copy_from_user(buffer, data, len) != 0) {
    ret = -EFAULT;
    goto done;
  }

  dma_sync_single_for_device(dev, dma, len, DMA_TO_DEVICE);

  struct dma_async_tx_descriptor *desc;

  desc = dmaengine_prep_slave_single(state->chan, dma, len, DMA_MEM_TO_DEV, 0);
  dmaengine_submit(desc);

  msleep(1000);
#else
  buffer = devm_kmalloc(state->dev, len, GFP_KERNEL);
  if (copy_from_user(buffer, data, len) != 0) {
    ret = -EFAULT;
    goto done;
  }

  for (int i=0; i<len; i++) {
    writel(buffer[i], state->regs);
  }
#endif
done:
#if 1
  //dma_free_coherent(dev, len, buffer, dma);
  dma_free_noncoherent(dev, len, buffer, dma, DMA_TO_DEVICE);
#else
  devm_kfree(state->dev, buffer);
#endif
  return len;
}

static int example_release(struct inode *inode, struct file *file) {
  file->private_data = NULL;
  return 0;
}

static struct file_operations char_fops = {
  .owner = THIS_MODULE,
  .open = example_open,
  .write = example_write,
  .release = example_release,
};

static int example_probe(struct platform_device *pdev) {
  struct example_state *state;
  struct device * dev = &pdev->dev;
  struct resource *mem;

  state = (struct example_state*) devm_kmalloc(dev, sizeof(struct example_state), GFP_KERNEL);
  state->dev = dev;
  if (!state) {
    dev_err(dev, "Couldnt allocate state\n");
    return -ENOMEM;
  }

  state->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &mem);

  writel('U', state->regs);

  state->chardev = cdev_alloc();
  // TODO, check for null

  state->chardev->owner = THIS_MODULE;
  state->chardev->ops = &char_fops;

  state->chan = dma_request_chan(dev, "tx");

  struct dma_slave_config tx_conf = {
    .dst_addr = state->regs,
    .dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
    .src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
    .direction = DMA_MEM_TO_DEV,
    .dst_maxburst = 2,
    .dst_port_window_size = 1,
    .device_fc = true,
  };

  dmaengine_slave_config(state->chan, &tx_conf);

  alloc_chrdev_region(&characterDevice,0,1,"example");

  cdev_add(state->chardev, characterDevice, 1);

  dev_set_drvdata(dev, state);
  gs = state;

  printk(KERN_INFO"example driver loaded\n");
  return 0;
}

static int example_remove(struct platform_device *pdev) {
  struct device * dev = &pdev->dev;
  struct example_state *state = dev_get_drvdata(dev);
  printk(KERN_INFO"example driver unloaded\n");

  cdev_del(state->chardev);
  unregister_chrdev_region(characterDevice, 1);
  dma_release_channel(state->chan);

  gs = NULL;
  devm_kfree(dev, state);
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

