/* LD_PRELOAD IO tracer for the low-LOD-tier hunt: logs every open of a game
 * asset and every pread64/read on those fds with wall latency, to the file
 * named by RESTUFF_IOTRACE (append). Answers, at syscall level, whether a
 * low-tier boot's guest STOPS requesting asset reads (gave up) or keeps
 * grinding slowly (starved).
 *
 *   gcc -O2 -shared -fPIC -o iotrace_shim.so iotrace_shim.c -ldl
 *   env LD_PRELOAD=.../iotrace_shim.so RESTUFF_IOTRACE=/tmp/iotrace.txt ./restuff
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static int (*real_openat)(int, const char*, int, ...);
static ssize_t (*real_pread64)(int, void*, size_t, off_t);
static ssize_t (*real_read)(int, void*, size_t);

static FILE* out;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned char tracked[65536];

static void init(void) {
  if (real_openat) return;
  real_openat = dlsym(RTLD_NEXT, "openat");
  real_pread64 = dlsym(RTLD_NEXT, "pread64");
  real_read = dlsym(RTLD_NEXT, "read");
  const char* p = getenv("RESTUFF_IOTRACE");
  if (p) out = fopen(p, "a");
}

static double now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, 0);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static void logf_(const char* fmt, ...) {
  if (!out) return;
  pthread_mutex_lock(&mu);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(out, fmt, ap);
  va_end(ap);
  fflush(out);
  pthread_mutex_unlock(&mu);
}

static void track(int fd, const char* path) {
  if (fd < 0 || fd >= 65536) return;
  if (path && (strstr(path, "/lu/") || strstr(path, "/streams/") || strstr(path, ".xex") ||
               strstr(path, ".lu") || strstr(path, ".cu"))) {
    tracked[fd] = 1;
    logf_("%.3f OPEN fd=%d %s\n", now_ms(), fd, path);
  } else {
    tracked[fd] = 0;
  }
}

int openat(int dirfd, const char* path, int flags, ...) {
  init();
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
  }
  int fd = real_openat(dirfd, path, flags, mode);
  track(fd, path);
  return fd;
}

int openat64(int dirfd, const char* path, int flags, ...) {
  init();
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
  }
  int fd = real_openat(dirfd, path, flags, mode);
  track(fd, path);
  return fd;
}

static int (*real_open)(const char*, int, ...);

int open(const char* path, int flags, ...) {
  init();
  if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
  }
  int fd = real_open(path, flags, mode);
  track(fd, path);
  return fd;
}

int open64(const char* path, int flags, ...) {
  init();
  if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
  }
  int fd = real_open(path, flags, mode);
  track(fd, path);
  return fd;
}

ssize_t pread64(int fd, void* buf, size_t n, off_t off) {
  init();
  if (!(fd >= 0 && fd < 65536 && tracked[fd])) return real_pread64(fd, buf, n, off);
  double t0 = now_ms();
  ssize_t r = real_pread64(fd, buf, n, off);
  double dt = now_ms() - t0;
  logf_("%.3f PREAD fd=%d off=%lld n=%zu ret=%zd ms=%.3f\n", t0, fd, (long long)off, n, r, dt);
  return r;
}

ssize_t read(int fd, void* buf, size_t n) {
  init();
  if (!(fd >= 0 && fd < 65536 && tracked[fd])) return real_read(fd, buf, n);
  double t0 = now_ms();
  ssize_t r = real_read(fd, buf, n);
  double dt = now_ms() - t0;
  logf_("%.3f READ fd=%d n=%zu ret=%zd ms=%.3f\n", t0, fd, n, r, dt);
  return r;
}
