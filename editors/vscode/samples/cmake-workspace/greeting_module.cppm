module;

#include <string>
#include <string_view>

export module sample.greeting;

export namespace sample {

std::string build_module_greeting(std::string_view name) {
    return "Hello, " + std::string(name) + " from the C++20 module sample.";
}

}
