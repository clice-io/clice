/// Adds two vectors, one work item per element.
__kernel void add(__global const float *a, __global const float *b, __global float *out) {
    size_t i = get_global_id(0);
    float4 lanes = vload4(i, a);
    out[i] = a[i] + b[i] + lanes.x;
}
