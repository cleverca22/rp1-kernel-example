#pragma once

struct example_state {
  struct cdev *chardev;
  void *regs;
  struct device * dev;
  struct dma_chan *tx_chan;
  struct dma_chan *rx_chan;
  struct file *open_handle;
  struct dma_async_tx_descriptor *desc;
  dma_addr_t dma;
  char *buffer;
  int rx_ring_cookie;
  wait_queue_head_t wait_queue;
  bool chunk_received;
  uint64_t read_ptr;
};

struct dma_packet_in_progress {
  wait_queue_head_t wait_queue;
  bool dma_done;
};
