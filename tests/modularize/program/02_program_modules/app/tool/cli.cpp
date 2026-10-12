#include "tool/tool.h"
signed int main() {
    const char* code = R"(int main() {})";
    const char* usage = R"(clice


run)";
    return code[0] == 'i' ? 0 : tool::run(core::Text{}, nullptr);
}
