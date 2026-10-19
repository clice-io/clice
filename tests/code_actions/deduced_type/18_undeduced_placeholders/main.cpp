template <typename T>
concept Small = sizeof(T) <= 4;

auto later();

void wait() {
    §(no_initializer)auto pending;
    §(constrained)Small auto counted;
    §(undeduced_call)auto result = later();
}
