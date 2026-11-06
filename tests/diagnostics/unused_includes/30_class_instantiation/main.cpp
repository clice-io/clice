#include "buffer.h"
#include "limits_int.h"
#include "unused.h"

int use() {
    Buffer<int> buffer{};
    return sizeof(buffer.data);
}
