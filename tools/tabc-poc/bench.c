// SPDX-License-Identifier: GPL-2.0-or-later
// The compiled benchmark of docs/research/tabc-poc.md: fixtures/bench's five workloads (calls, bytes, sort, float,
// random loads) in C, so that one source is built for x86-64 (native, PISCO) and aarch64 (tabc) by one compiler.
// Prints each workload's time in ms (CLOCK_MONOTONIC) and its checksum; the checksums are the same on every target.
// bench [workload...]: only those (0 to 4), each run once.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BYTES (64u << 20)
#define WORDS (2u << 20)
#define TABLE (4u << 20)
#define VECS (1u << 20)
static uint8_t *bytes; static uint32_t *words, *table; static float (*vecs)[4];

__attribute__((noinline)) static uint64_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
__attribute__((noinline)) static uint64_t fnv(const uint8_t *d, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull; for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 0x100000001b3ull; } return h;
}
static void fill(uint32_t *d, size_t n, uint32_t s) { for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; d[i] = s; } }
static void sift(uint32_t *a, size_t i, size_t n) {
  for (;;) { size_t l = 2 * i + 1, m = l; if (l >= n) break; if (l + 1 < n && a[l + 1] > a[l]) m = l + 1; if (a[i] >= a[m]) break;
    uint32_t t = a[i]; a[i] = a[m]; a[m] = t; i = m; }
}
__attribute__((noinline)) static void sort(uint32_t *a, size_t n) {   // heapsort: data-dependent branches and loads
  for (size_t i = n / 2; i-- > 0;) sift(a, i, n);
  for (size_t e = n - 1; e > 0; e--) { uint32_t t = a[0]; a[0] = a[e]; a[e] = t; sift(a, 0, e); }
}
__attribute__((noinline)) static void transform(float (*v)[4], size_t n, const float m[4][4]) {
  for (size_t i = 0; i < n; i++) { float q[4]; memcpy(q, v[i], sizeof q);
    for (int r = 0; r < 4; r++) v[i][r] = m[r][0] * q[0] + m[r][1] * q[1] + m[r][2] * q[2] + m[r][3] * q[3]; }
}
__attribute__((noinline)) static uint32_t gather(const uint32_t *t, uint32_t mask, uint32_t steps) {   // random access over a large table
  uint32_t i = 1, acc = 0; for (uint32_t s = 0; s < steps; s++) { i = t[i & mask] ^ i * 2654435761u; acc += i; } return acc;
}
static uint64_t run(int k) {
  switch (k) {
  case 0: return fib(32);
  case 1: for (size_t i = 0; i < BYTES; i++) bytes[i] = (uint8_t)(i * 131); return fnv(bytes, BYTES);
  case 2: fill(words, WORDS, 12345); sort(words, WORDS); return (uint64_t)words[0] << 32 | (words[WORDS - 1] ^ words[WORDS / 2]);
  case 3: {
    for (size_t i = 0; i < VECS; i++) { vecs[i][0] = (float)i; vecs[i][1] = 1; vecs[i][2] = 2; vecs[i][3] = 1; }
    static const float m[4][4] = { { 0.5f, 0.1f, 0, 1 }, { 0, 0.9f, 0.2f, 2 }, { 0.3f, 0, 0.7f, 3 }, { 0, 0, 0, 1 } };
    for (int j = 0; j < 8; j++) transform(vecs, VECS, m);
    float s = 0; for (size_t i = 0; i < VECS; i++) s += vecs[i][0] + vecs[i][1] + vecs[i][2];
    uint32_t b; memcpy(&b, &s, 4); return b;
  }
  default: fill(table, TABLE, 777); return gather(table, TABLE - 1, 20000000);
  }
}
static const char *names[5] = { "calls   fib(32)", "bytes   fnv 64 MB", "sort    heapsort 2M u32", "float   4x4 transform 8x1M", "memory  20M random loads" };
int main(int argc, char **argv) {
  bytes = malloc(BYTES); words = malloc(WORDS * 4); table = malloc(TABLE * 4); vecs = malloc(VECS * 16);
  if (!bytes || !words || !table || !vecs) return 98;
  int ks[5] = { 0, 1, 2, 3, 4 }, n = 5;
  if (argc > 1) { n = 0; for (int i = 1; i < argc && n < 5; i++) ks[n++] = atoi(argv[i]) % 5; }
  for (int j = 0; j < n; j++) {
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a); uint64_t r = run(ks[j]); clock_gettime(CLOCK_MONOTONIC, &b);
    long us = (b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000;
    printf("%s  %ld ms  checksum %016llx\n", names[ks[j]], us / 1000, (unsigned long long)r);
  }
  return 0;
}
