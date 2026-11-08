#if !__has_embed("data.bin")
#error data.bin cannot be embedded
#endif

constexpr unsigned char checked[] = {
#embed "data.bin"
};

static_assert(sizeof(checked) == 5);
