// How long ONE durability barrier costs on this device, which is the only
// honest basis for how often to place one. Measured three ways, because the
// answer is expected to be dominated by the filesystem commit rather than by
// the number of bytes: a barrier over 4 KB and one over 1 MB should cost about
// the same, and if they do, grouping by TIME is right and grouping by bytes is
// arbitrary.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
static int cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static void run(const char* label, size_t dirty_bytes, int iters, char* base, int fd) {
    double* v = malloc(sizeof(double) * iters);
    size_t off = 0;
    for (int i = 0; i < iters; ++i) {
        for (size_t p = 0; p < dirty_bytes; p += 4096) base[off + p] = (char)(i + 1);
        double t0 = now_us();
        msync(base + off, dirty_bytes, MS_SYNC);
        v[i] = now_us() - t0;
        off += dirty_bytes;
    }
    qsort(v, iters, sizeof(double), cmp);
    double sum = 0; for (int i = 0; i < iters; ++i) sum += v[i];
    printf("  %-18s n=%3d  mean %8.1f us   p50 %8.1f   p99 %8.1f   -> %6.0f barriers/sec\n",
           label, iters, sum / iters, v[iters/2], v[(int)(iters*0.99)], 1e6 / (sum / iters));
    free(v);
    (void)fd;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "msync_probe.tmp";
    size_t cap = 768ull << 20;
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    if (posix_fallocate(fd, 0, cap) != 0) { perror("fallocate"); return 1; }
    char* base = mmap(NULL, cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }

    printf("msync latency on %s\n", path);
    run("4 KB dirty", 4096, 200, base, fd);
    run("64 KB dirty", 65536, 200, base + (16ull<<20), fd);
    run("1 MB dirty", 1u<<20, 100, base + (48ull<<20), fd);
    run("8 MB dirty", 8u<<20, 30, base + (400ull<<20), fd);

    munmap(base, cap);
    close(fd);
    unlink(path);
    return 0;
}
