struct File;
struct Stream;

extern File* error_file;

template <typename... Args>
void print(Stream& stream, const char* format, Args&&... args);

template <typename... Args>
void print(File* file, const char* format, Args&&... args);

template <typename Format, typename... Args>
void print(Format format, Args&&... args);

template <typename T>
void report(T value) {
    §(call)print(error_file, "{}", value);
}

struct Path {
    operator const char*() const;
};

void open(const char* name, int flags);
void open(int descriptor, int flags);

template <typename T>
void reopen(const Path& path, T flags) {
    §(path)open(path, flags);
    §(descriptor)open(2, flags);
}
