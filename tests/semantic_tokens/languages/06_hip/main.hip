#define __global__ __attribute__((global))
#define __device__ __attribute__((device))

struct dim3 {
    dim3(unsigned x = 1, unsigned y = 1, unsigned z = 1) : x(x), y(y), z(z) {}
    unsigned x, y, z;
};

using hipStream_t = struct ihipStream_t*;

extern "C" int __hipPushCallConfiguration(dim3 grid,
                                          dim3 block,
                                          unsigned long shared = 0,
                                          hipStream_t stream = nullptr);

__device__ float twice(float value) {
    return value * 2.0f;
}

/// Doubles every element, one thread each.
__global__ void scale(float* values) {
    values[0] = twice(values[0]);
}

void launch(float* values) {
    scale<<<dim3(1), dim3(64)>>>(values);
}
