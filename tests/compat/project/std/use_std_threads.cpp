import std;

int count_threads() {
    std::jthread worker([] {});
    return static_cast<int>(std::thread::hardware_concurrency() > 0);
}
