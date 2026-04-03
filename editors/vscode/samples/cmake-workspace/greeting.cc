#include "greeting.h"

#include <string>

std::string build_greeting(std::string_view name) {
    return "Hello, " + std::string(name) + " from the CMake sample.";
}
