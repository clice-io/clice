template <typename... Args>
void emit(Args... args);

template <typename... Args>
void log_at(int level, Args... args);

template <typename... Ts>
struct Tuple {};

int main() {
    emit(§(pos));
    emit(1, §(second)2);
    log_at(0, 1, §(after_level)2);
    Tuple<int, char, §(template_pack)double> tuple;
}
