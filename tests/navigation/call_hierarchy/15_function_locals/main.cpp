int total(int §(parameter)count) {
    int §(local)sum = 0;
    for(int i = 0; i < count; i += 1) {
        sum += i;
    }
    return sum;
}

void run() {
    struct Counter {
        int §(field)hits = 0;

        void bump() {
            hits += 1;
        }
    };
}
