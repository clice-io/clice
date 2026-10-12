// Designators survive next to initializers that fail to compile: the first
// one here does not convert.
struct Empty {};

struct Mixed {
    int a;
    int b;
};

void use() {
    Mixed m{Empty(), 1};
}
