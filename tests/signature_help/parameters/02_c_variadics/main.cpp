void record(int code, ...);
void (*logger)(const char* format, ...);

int main() {
    record(§(pos)0);
    record(0, §(extra)1);
    logger("%d", §(pointer)1);
}
