constexpr unsigned char checked[] = {
#if __has_embed("data.bin")
#embed "data.bin"
#endif
};

static_assert(sizeof(checked) == 5);
