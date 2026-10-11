void reset(int& value);
void inspect(const int& value);
void copy(int value);
void exchange(int& lhs, int& rhs);

struct Gauge {
    Gauge(int& source);
};

void run() {
    int §(level)level = 0;
    int other = 1;
    reset(level);
    inspect(level);
    copy(level);
    exchange(level, other);
    Gauge gauge(level);
    int* pointer = &level;
}
