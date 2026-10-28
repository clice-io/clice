struct Counter {
    int §(member)count;
    void §(method)bump();
};

int §(global)limit = 10;

template <class T>
T §(templated)twice(T value);

void run(Counter& counter) {
    counter.§(call)bump();
    int §(local)next = counter.count + 1;
    §(ret)return;
}
