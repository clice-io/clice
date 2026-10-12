namespace detail {

void open_file(const char* path, int mode = 0644);

struct Server {
    void listen(int port = 8080, int backlog = 128);
};

}  // namespace detail
