#if __has_include("kernel.cu.expect.h")
#include "kernel.cu.expect.h"
#endif

#include "util.h"

__global__ void scale(float* p) {
    p[threadIdx.x] *= 2.0f;
}

int main() {
    float* d = nullptr;
    cudaMalloc(&d, 16);
    scale<<<1, 4>>>(d);
    return util_add(0, 0);
}
