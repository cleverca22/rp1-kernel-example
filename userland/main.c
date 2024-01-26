#define _GNU_SOURCE

#include <assert.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// based on https://git.kernel.dk/cgit/liburing/tree/examples/io_uring-cp.c

#define QD 64

struct io_data {
  int read;
  off_t first_offset, offset;
  size_t first_len;
  struct iovec iov;
  struct timespec read_queued, read_done, write_done;
};

static int pending_reads = 0;
static int pending_writes = 0;
static int write_offset = 0;

static int queue_read(struct io_uring *ring, int pio_fd, off_t size) {
  struct io_uring_sqe *sqe;
  struct io_data *data;
  data = malloc(size + sizeof(*data));
  if (!data) return 1;

  sqe = io_uring_get_sqe(ring);
  if (!sqe) {
    free(data);
    return 1;
  }

  data->read = 1;
  data->offset = data->first_offset = 0;
  data->iov.iov_base = data+1;
  data->iov.iov_len = size;
  data->first_len = size;

  io_uring_prep_readv(sqe, pio_fd, &data->iov, 1, 0);
  io_uring_sqe_set_data(sqe, data);

  //puts("read queued");
  clock_gettime(CLOCK_MONOTONIC, &data->read_queued);
  pending_reads++;

  return 0;
}

static void queue_prepped(struct io_uring *ring, int out_fd, struct io_data *data) {
  struct io_uring_sqe *sqe;

  sqe = io_uring_get_sqe(ring);
  assert(sqe);

  io_uring_prep_writev(sqe, out_fd, &data->iov, 1, data->offset);

  io_uring_sqe_set_data(sqe, data);
}

static void queue_write(struct io_uring *ring, int out_fd, struct io_data *data) {
  data->read = 0;
  data->iov.iov_base = data + 1;
  data->iov.iov_len = data->first_len;
  data->offset = write_offset;
  // write_offset += data->iov.iov_len;

  queue_prepped(ring, out_fd, data);
  pending_writes++;
  io_uring_submit(ring);
}

uint64_t timediff(struct timespec *past, struct timespec *future) {
  uint64_t sec = future->tv_sec - past->tv_sec;
  uint64_t nsec = future->tv_nsec - past->tv_nsec;
  return nsec + (sec * 1000000000);
}

static int setup_child(int output, int blocksize) {
  int fds[2];
  pipe(fds);
  int ret = fcntl(fds[1], F_SETPIPE_SZ, blocksize);
  if (ret < 0) {
    perror("F_SETPIPE_SZ failed");
    exit(-1);
  }
  ret = fork();
  if (ret == 0) { // child
    close(fds[1]);

    dup2(output, 1);
    dup2(fds[0], 0);
    for (int i=3; i<20; i++) close(i);

    char *argv[] = { "-9v", NULL };
    execvp("gzip", argv);
    return -1;
  } else {
    close(fds[0]);
    return fds[1];
  }
}

int main(int argc, char **argv) {
  struct io_uring ring;

  int ret = io_uring_queue_init(QD, &ring, 0);
  if (ret < 0) {
    perror("io_uring_queue_init failed\n");
    return -1;
  }

  int pio_fd = open("/dev/example", O_RDONLY);
  if (pio_fd < 0) {
    perror("cant open /dev/example\n");
    return -1;
  }

  int out_file_fd = open("output.bin.gz", O_WRONLY | O_CREAT, 0644);
  if (out_file_fd < 0) {
    perror("cant open output\n");
    return -1;
  }

  int out_fd;
  int blocksize = 1024 * 1024 * 10;
  if (true) {
    out_fd = setup_child(out_file_fd, blocksize);
    if (out_fd < 0) {
      perror("cant setup child");
      return -1;
    }
  } else {
    out_fd = out_file_fd;
  }


  int concurrent_reads = 10;

  for (int i=0; i<concurrent_reads; i++) {
    queue_read(&ring, pio_fd, blocksize);
  }

  ret = io_uring_submit(&ring);
  if (ret < 0) {
    perror("cant io_uring_submit\n");
    return -1;
  }

  int toread = blocksize * concurrent_reads;
  int towrite = toread;

  while ((toread > 0) || (towrite > 0)) {
    struct io_uring_cqe *cqe;

    //printf("toread %d, towrite %d\n", toread, towrite);

    ret = io_uring_wait_cqe(&ring, &cqe);
    //printf("0x%lx\n", (uint64_t)cqe);
    if (ret < 0) {
      perror("cant io_uring_wait_cqe\n");
      return -1;
    }
    struct io_data *data = io_uring_cqe_get_data(cqe);
    if (cqe->res < 0) {
      printf("async IO failed %d\n", cqe->res);
      printf("read? %d\n", data->read);
      return -1;
    } else if (cqe->res != data->iov.iov_len) {
      printf("error, asked for %ld, got %d\n", data->iov.iov_len, cqe->res);
      return -1;
    }

    if (data->read) {
      pending_reads--;
      toread -= cqe->res;
      //puts("read completed");
      clock_gettime(CLOCK_MONOTONIC, &data->read_done);
      queue_write(&ring, out_fd, data);
    } else {
      pending_writes--;
      towrite -= cqe->res;
      clock_gettime(CLOCK_MONOTONIC, &data->write_done);
      //printf("WD %ld %ld %ld\n", data->read_queued.tv_nsec, data->read_done.tv_nsec, data->write_done.tv_nsec);
      double readtime = timediff(&data->read_queued, &data->read_done);
      readtime = readtime / 1000 / 1000 / 1000;
      double totaltime = timediff(&data->read_queued, &data->write_done);
      totaltime = totaltime / 1000 / 1000 / 1000;

      double write_time = timediff(&data->read_done, &data->write_done);
      write_time = write_time / 1000 / 1000 / 1000;

      // because $concurrent_reads of backlog exist in the uring, it takes $concurrent_reads times longer, for a read to go from being issued, to returning a result
      double bytes_per_sec = (blocksize / totaltime) * concurrent_reads;
      double bits_per_sec = bytes_per_sec * 8;
      printf("WD %f %f, %f MB, %f Mbit, pending %d %d\n", readtime, write_time, bytes_per_sec/1024/1024, bits_per_sec/1000/1000, pending_reads, pending_writes);
#if 1
      queue_read(&ring, pio_fd, blocksize);
      toread += blocksize;
      ret = io_uring_submit(&ring);
      if (ret < 0) {
        perror("cant io_uring_submit\n");
        return -1;
      }
#endif
    }
    io_uring_cqe_seen(&ring, cqe);
  }

  io_uring_queue_exit(&ring);
  return 0;
}
