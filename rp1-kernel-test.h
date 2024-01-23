#pragma once

struct example_state {
  struct cdev *chardev;
  void *regs;
  struct device * dev;
  struct dma_chan *chan;
};
