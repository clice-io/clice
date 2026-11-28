template <typename T>
concept Small = sizeof(T) <= 8;

#ifdef FROM_PROJECT
#error the local file removes FROM_PROJECT
#endif

int twice(Small auto value) {
    return value + value;
}

int main() {
    return twice(1) - 2;
}
