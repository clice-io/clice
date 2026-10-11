// A macro whose expansion runs a `_Pragma` operator offers no expansion: the
// pragma leaves no tokens behind to write in its place.

#define PACKED_BEGIN _Pragma("pack(push, 1)")
#define PACKED_END _Pragma("pack(pop)")
#define DO_PRAGMA(x) _Pragma(#x)
#define WARNINGS_PUSH DO_PRAGMA(GCC diagnostic push)
#define WARNINGS_POP DO_PRAGMA(GCC diagnostic pop)

§(direct)PACKED_BEGIN
struct Header {
    char tag;
    int size;
};
PACKED_END

§(nested)WARNINGS_PUSH
int quiet;
WARNINGS_POP
