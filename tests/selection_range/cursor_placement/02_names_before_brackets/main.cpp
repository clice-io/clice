struct Buffer {
    int values[4];
    void flush§(call)();
};

int first(Buffer& buffer) {
    buffer.flush()§(statement);
    return buffer.values§(subscript)[0];
}
