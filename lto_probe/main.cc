#include <cstdio>

int lto_probe_lib(int);

int main(int argc, char**) {
    std::printf("%d\n", lto_probe_lib(argc));
}
