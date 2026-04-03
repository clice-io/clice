#include <iostream>

import sample.greeting;

int main() {
    std::cout << sample::build_module_greeting("clice") << '\n';
    return 0;
}
