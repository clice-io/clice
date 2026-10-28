void configure(
    int width,
    int height,
    bool fullscreen
);

int compute(int a, int b, int c);

void demo() {
    int values[] = {
        1,
        2,
        3
    };

    int result = compute(
        values[0],
        values[1],
        values[2]
    );

    auto sum = [
        first = values[0],
        second = values[1]
    ] {
        return first + second;
    };

    auto scale = [](
        int base,
        int factor
    ) {
        return base * factor;
    };

    result += sum() + scale(result, 2);
}

int accumulate(
    int start,
    int step,
    int count
) {
    return start + step * count;
}

void log_all(
    const char* format,
    ...
);

struct Rect {
    Rect(int w, int h);
};

Rect area(
    10,
    20
);

Rect brace_area{
    30,
    40
};
