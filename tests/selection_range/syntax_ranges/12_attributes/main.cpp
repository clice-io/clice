struct alignas(§(alignas)16) Vector {
    float x, y;
};

[[deprecated("use §(message)other")]] int old_api();

int counter __attribute__((§(gnu)aligned(8)));

[[using gnu: §(using)hot]] void warm();
