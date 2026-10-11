#if __has_include("config.h")
#error config.h is there
#endif

#if __has_embed("data.bin")
#error data.bin is there
#endif

int main() {
    return 0;
}
