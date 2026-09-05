/// # Default argument stripping
///
/// - status: supported
/// - issues: clangd#221
///
/// The signature is derived from the function type, so default parameter values never leak into the outline

namespace detail {

void open_file(const char* path, int mode = 0644);

struct Server {
    void listen(int port = 8080, int backlog = 128);
};

}  // namespace detail
