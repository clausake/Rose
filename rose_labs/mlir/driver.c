#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

extern int64_t remplir_hasard(void *a, void *b, int64_t off, int64_t sz, int64_t stride, int64_t n, int64_t state);

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    size_t N = 12500000;
    if (argc > 1) N = (size_t)atol(argv[1]);
    unsigned char *buf = (unsigned char *)malloc(N * sizeof(int64_t));
    if (!buf) { perror("malloc"); return 1; }
    int64_t state = 12345;
    // warmup
    remplir_hasard(buf, buf, 0, (int64_t)N, 1, (int64_t)N, state);
    double best = 1e9;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ms();
        state = remplir_hasard(buf, buf, 0, (int64_t)N, 1, (int64_t)N, state);
        double t1 = now_ms();
        double dt = t1 - t0;
        if (dt < best) best = dt;
        printf("pass %d: %.1f ms (state=%ld)\n", r, dt, (long)state);
    }
    printf("min: %.1f ms\n", best);
    free(buf);
    return 0;
}