#pragma once

struct example_state {
  struct cdev *chardev;
  void *regs;
  struct device * dev;
  struct dma_chan *tx_chan;
  struct dma_chan *rx_chan;
};

struct dma_packet_in_progress {
  wait_queue_head_t wait_queue;
  bool dma_done;
};
