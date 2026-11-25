#if __has_embed("mark.bin") == __STDC_EMBED_FOUND__
constexpr bool found = true;
#endif

static_assert(found);
