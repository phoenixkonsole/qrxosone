#ifndef QRX_BENCH_SCENE_H
#define QRX_BENCH_SCENE_H
/* SPDX-License-Identifier: MIT — QRX benchmark scene, API-neutral 800x450 workload. */
#include "qrx_accel.h"
#include <stddef.h>
#include <stdint.h>
#define QRX_BENCH_W 800u
#define QRX_BENCH_H 450u
#define QRX_BENCH_TRIANGLES 240u
/* 1 clear, 240 opaque colored triangles and 16 transparent overlays. */
#define QRX_BENCH_COMMANDS (1u+QRX_BENCH_TRIANGLES+16u)
void qrx_bench_triangle(unsigned index, QRXAccelVertex out[3]);
QRXAccelFrame *qrx_bench_scene(void);
/* Bounded CPU correctness reference, RGBA8 top-to-bottom (0 on success). */
int qrx_bench_reference(const QRXAccelFrame *frame, uint8_t *out, size_t n);
int qrx_bench_save_ppm(const char *path, const uint8_t *rgba);
uint64_t qrx_bench_hash(const uint8_t *rgba, size_t len);
#endif
