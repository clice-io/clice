/// # `__has_embed` — the checked path links to the probed resource
///
/// - status: supported

#if __has_embed("data.bin")
const char first_byte[] = {
#embed "data.bin" limit(1)
};
#endif
