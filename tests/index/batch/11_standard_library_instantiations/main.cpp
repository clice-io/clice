#include <algo>

struct Task {
    void go();
};

void use() {
    std::call(Task{});
}
