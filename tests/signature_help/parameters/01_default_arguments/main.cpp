#define DEFAULT_TIMEOUT 30
#define DECLARE_RETRY(name) void name(int attempts = 3)

void configure(int width, int height = 100, bool visible = true);
void open_file(const char* path, int mode = 0644, int timeout = DEFAULT_TIMEOUT);
void listen(int port = 8000 +
                       80);
DECLARE_RETRY(retry);

int main() {
    configure(§(pos)1);
    open_file("data.txt", §(as_written)0600);
    listen(§(several_lines)8080);
    retry(§(macro_body)5);
}
