// Two headers whose names are letters clang takes in an identifier but no
// ASCII ones: each partition keeps its file's name.
#include "core/文本.h"
#include "core/数据.h"

int main() {
    return core::Text{}.size() + core::Data{}.count();
}
