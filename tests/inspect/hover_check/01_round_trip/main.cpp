#define NODISCARD [[nodiscard]]
#define PACKED __attribute__((packed))

namespace round_trip {
struct Empty {};

struct Base {
    int base_value;
};

NODISCARD int compute();
[[deprecated("use run_v2")]] void run();
void fail() __attribute__((noreturn, cold));
int counter [[maybe_unused]] = 0;
template <typename T> [[nodiscard]] T make();

struct PACKED Header {
    char tag;
    int size;
};

struct Record {
    static struct {
        int count;
    } shared;
    alignas(8) union {
        int bits;
        float real;
    } aligned[2];
    struct {
        int x;
        int y;
    } position, *previous;
    enum { capacity = sizeof(long) };
    typedef struct {
        int first;
        int second;
    } Pair;
    using Extent = struct {
        long length;
    };
    [[no_unique_address]] Empty empty;
    int (*callback)(int) = [](int value) { return value + 1; };
};

typedef struct {
    int x;
    int y;
} Point;

int spread = 1 +
             2 /* two */ +
             3;
const char* raw = R"(a "quoted"
line)";
int values[] = {1, 2, 3};
inline constexpr int limit = 4;

template <typename T, int Size> struct alignas(64) Cache final : Base {
    struct Node;
    enum Kind : int;
    using value_type = T;
    [[no_unique_address]] Empty tag;
    alignas(32) T slots[Size];
    value_type* cursor = nullptr;
};

Cache<long, 4> cache;

template <typename T, T Value> struct Constant {
    static constexpr T value = Value;
};

Constant<int, 7> seven;
}
