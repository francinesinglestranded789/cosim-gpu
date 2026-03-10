#pragma once

#include <hip/hip_runtime.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

struct StreamConfig {
    StreamConfig() {
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
};

static StreamConfig g_stream_config;

// Check HIP API calls
#define HIP_CHECK(call)                                                  \
    do {                                                                 \
        hipError_t err = (call);                                         \
        if (err != hipSuccess) {                                         \
            fprintf(stderr, "HIP error at %s:%d: %s\n", __FILE__,       \
                    __LINE__, hipGetErrorString(err));                    \
            return 1;                                                    \
        }                                                                \
    } while (0)

// Verify condition, print PASS/FAIL
#define VERIFY(name, cond)                                               \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("  FAIL: %s\n", name);                                \
            failures++;                                                  \
        } else {                                                         \
            printf("  PASS: %s\n", name);                                \
        }                                                                \
    } while (0)

// Compare floating-point arrays with tolerance
static inline int check_float(const float* ref, const float* out,
                               int n, float tol = 1e-3f) {
    int errs = 0;
    for (int i = 0; i < n; i++) {
        float diff = fabsf(ref[i] - out[i]);
        float scale = fmaxf(fabsf(ref[i]), 1.0f);
        if (diff / scale > tol) {
            if (errs < 5)
                printf("    mismatch [%d]: ref=%.6f got=%.6f\n",
                       i, ref[i], out[i]);
            errs++;
        }
    }
    if (errs > 5)
        printf("    ... and %d more mismatches\n", errs - 5);
    return errs;
}

// Compare integer arrays
static inline int check_int(const int* ref, const int* out, int n) {
    int errs = 0;
    for (int i = 0; i < n; i++) {
        if (ref[i] != out[i]) {
            if (errs < 5)
                printf("    mismatch [%d]: ref=%d got=%d\n",
                       i, ref[i], out[i]);
            errs++;
        }
    }
    if (errs > 5)
        printf("    ... and %d more mismatches\n", errs - 5);
    return errs;
}

// Wall-clock timer
struct Timer {
    std::chrono::high_resolution_clock::time_point t0;
    void start() { t0 = std::chrono::high_resolution_clock::now(); }
    double elapsed_ms() {
        auto t1 = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
};

// Test result summary
static inline void print_summary(const char* test_name, int failures,
                                  double ms) {
    const char* status = (failures == 0) ? "PASS" : "FAIL";
    printf("[%s] %s (%.1f ms)\n", status, test_name, ms);
}
