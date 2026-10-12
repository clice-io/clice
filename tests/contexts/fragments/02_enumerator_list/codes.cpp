enum class Code {
#define ERROR(name, value) name = value,
#include "errors.def"
#undef ERROR
};

const char* text(Code c) {
    switch (c) {
#define ERROR(name, value) \
    case Code::name: return #name;
#include "errors.def"
#undef ERROR
    }
    return "";
}
