#if !__has_embed("data.bin")
#error data.bin cannot be embedded
#endif

constexpr unsigned char probed[] = {
#embed "data.bin"
};

static_assert(sizeof(probed) == 5);
