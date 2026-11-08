enum class Code {
#define ERROR(name, value) name = value,
#include "errors.def"
#undef ERROR
};
