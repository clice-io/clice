/// # `__has_embed`
///
/// - status: supported
///
/// The checked path links to the probed resource

#if __has_embed("data.bin")
const char first_byte[] = {
#embed "data.bin" limit(1)
};
#endif
