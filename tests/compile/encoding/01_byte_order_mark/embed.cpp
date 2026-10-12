constexpr unsigned char data[] = {
#embed "data.bin"
};

static_assert(sizeof(data) == 5);
