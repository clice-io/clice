typedef unsigned int __u32;
typedef float __v4sf __attribute__((__vector_size__(16)));

__u32 checksum();
__v4sf lanes();

void use() {
    auto sum = checksum();
    auto packed = lanes();
}
