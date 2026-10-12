// An omitted default argument too long for the hint budget is abbreviated.
struct Endpoint {
    const char* host;
    int port;
};

void connect(int retries, Endpoint target = Endpoint{"service.internal.example.com", 8080}, bool secure = false);

void use() {
    connect(3);
}
