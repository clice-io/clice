/// # Symbol Detail
///
/// ## Default argument stripping — the signature is derived from the function type, so default parameter values never leak into the outline
///
/// - status: supported
/// - issues: clangd#221
/// - order: 3

namespace detail {

void open_file(const char* path, int mode = 0644);

struct Server {
    void listen(int port = 8080, int backlog = 128);
};

}  // namespace detail
