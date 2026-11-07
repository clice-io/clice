#ifndef __CUDA_ARCH__
#error "the host job"
#endif

__attribute__((global)) void scale(float* values) {
    values[0] *= 2.0f;
}
