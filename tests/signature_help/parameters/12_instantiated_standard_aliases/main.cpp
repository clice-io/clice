#include <string>

template <typename T>
struct Box {
    void put(T value);
    void fill(T values[2]);
    void each(void (*visit)(T));
};

void use(Box<std::string> names, Box<std::pmr::string> pooled, std::string row[2]) {
    names.put(§(standard)row[0]);
    pooled.put(§(polymorphic){});
    names.fill(§(array)row);
    names.each(§(function_pointer)nullptr);
}
