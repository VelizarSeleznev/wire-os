#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <unistd.h>

static void try_one(uint32_t msg, size_t words) {
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    perror("socket");
    return;
  }

  struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  char local_path[108];
  snprintf(local_path, sizeof(local_path), "/tmp/camprobe-%d-%u.sock", getpid(), msg);
  unlink(local_path);
  struct sockaddr_un local;
  memset(&local, 0, sizeof(local));
  local.sun_family = AF_UNIX;
  snprintf(local.sun_path, sizeof(local.sun_path), "%s", local_path);
  if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
    printf("msg=%u bind failed: %s\n", msg, strerror(errno));
    close(fd);
    return;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", "/var/run/mm-anki-camera/camera-server");

  uint32_t payload[8] = { msg, 0, 0, 0, 0, 0, 0, 0 };
  errno = 0;
  ssize_t wn = sendto(fd, payload, words * sizeof(uint32_t), 0, (struct sockaddr *)&addr, sizeof(addr));
  printf("msg=%u words=%zu send bytes=%zd errno=%d\n", msg, words, wn, errno);

  char data[256];
  char ctrl[CMSG_SPACE(sizeof(int) * 4)];
  struct iovec iov = { .iov_base = data, .iov_len = sizeof(data) };
  struct msghdr mh;
  memset(&mh, 0, sizeof(mh));
  mh.msg_iov = &iov;
  mh.msg_iovlen = 1;
  mh.msg_control = ctrl;
  mh.msg_controllen = sizeof(ctrl);

  errno = 0;
  int received_fd = -1;
  ssize_t rn = recvmsg(fd, &mh, 0);
  printf("msg=%u recv bytes=%zd errno=%d flags=0x%x", msg, rn, errno, mh.msg_flags);
  if (rn > 0) {
    printf(" data=");
    for (ssize_t i = 0; i < rn && i < 64; ++i) printf("%02x", (unsigned char)data[i]);
  }
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int *fds = (int *)CMSG_DATA(c);
      int count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      printf(" fds=");
      for (int i = 0; i < count; ++i) {
        struct stat st;
        int size = -1;
        if (fstat(fds[i], &st) == 0) size = (int)st.st_size;
        printf("%d(size=%d)", fds[i], size);
        if (received_fd < 0) received_fd = fds[i];
        else close(fds[i]);
      }
    }
  }
  printf("\n");
  if (rn > 0 && received_fd >= 0) {
    uint32_t *w = (uint32_t *)data;
    size_t map_len = 0x800000;
    if (rn >= 20 && w[4] > 0 && w[4] < 64 * 1024 * 1024) map_len = w[4];
    void *map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, received_fd, 0);
    printf("msg=%u mmap len=%zu ptr=%p errno=%d\n", msg, map_len, map == MAP_FAILED ? NULL : map, errno);
    if (map != MAP_FAILED) {
      unsigned char *p = (unsigned char *)map;
      printf("mmap first64=");
      for (int i = 0; i < 64; ++i) printf("%02x", p[i]);
      printf("\n");
      for (int poll = 0; poll < 25; ++poll) {
        uint32_t *hw = (uint32_t *)p;
        printf("poll=%d header:", poll);
        for (int i = 0; i < 32; ++i) printf(" %08x", hw[i]);
        printf("\n");
        usleep(200000);
      }
      int out = open("/tmp/cammap.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (out >= 0) {
        size_t written = 0;
        while (written < map_len) {
          ssize_t n = write(out, p + written, map_len - written);
          if (n <= 0) break;
          written += (size_t)n;
        }
        close(out);
        printf("wrote /tmp/cammap.bin %zu/%zu bytes\n", written, map_len);
      }
    }
    for (int n = 0; n < 10; ++n) {
      memset(data, 0, sizeof(data));
      memset(ctrl, 0, sizeof(ctrl));
      mh.msg_controllen = sizeof(ctrl);
      errno = 0;
      rn = recvmsg(fd, &mh, 0);
      printf("event %d bytes=%zd errno=%d", n, rn, errno);
      if (rn > 0) {
        printf(" data=");
        for (ssize_t i = 0; i < rn && i < 64; ++i) printf("%02x", (unsigned char)data[i]);
      }
      printf("\n");
    }
    if (map != MAP_FAILED) munmap(map, map_len);
    close(received_fd);
  }
  close(fd);
  unlink(local_path);
}

int main(int argc, char **argv) {
  uint32_t msg = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 0) : 1;
  size_t words = argc > 2 ? (size_t)strtoul(argv[2], 0, 0) : 4;
  try_one(msg, words);
  return 0;
}
