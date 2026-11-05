module;
#include <string>
#include <string_view>
export module lib;

export struct sink {
    sink(std::string_view) {}
};

export struct server {
    std::string text;
    std::string_view name;

    void stop() {}
};
