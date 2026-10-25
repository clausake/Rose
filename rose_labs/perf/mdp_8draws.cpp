#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
static uint64_t etat = 2026;
static inline uint64_t tirer() {
    uint64_t z = (etat += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
int main(int argc, char **argv) {
    const char *ab = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    size_t n = argc > 1 ? strtoull(argv[1], nullptr, 10) : 100000000UL;
    timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    std::string buf(n, '\0');
    unsigned char *p = (unsigned char *)buf.data();
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        uint64_t r0 = tirer(), r1 = tirer(), r2 = tirer(), r3 = tirer();
        uint64_t r4 = tirer(), r5 = tirer(), r6 = tirer(), r7 = tirer();
        for (int k = 0; k < 8; k++) {
            p[i + k]      = (unsigned char)ab[(r0 >> (8 * k)) & 63];
            p[i + 8 + k]  = (unsigned char)ab[(r1 >> (8 * k)) & 63];
            p[i + 16 + k] = (unsigned char)ab[(r2 >> (8 * k)) & 63];
            p[i + 24 + k] = (unsigned char)ab[(r3 >> (8 * k)) & 63];
            p[i + 32 + k] = (unsigned char)ab[(r4 >> (8 * k)) & 63];
            p[i + 40 + k] = (unsigned char)ab[(r5 >> (8 * k)) & 63];
            p[i + 48 + k] = (unsigned char)ab[(r6 >> (8 * k)) & 63];
            p[i + 56 + k] = (unsigned char)ab[(r7 >> (8 * k)) & 63];
        }
    }
    for (; i < n; i++) p[i] = (unsigned char)ab[tirer() & 63];
    timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("%zu\n%ld ms\n", buf.size(),
        (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000);
    return 0;
}
