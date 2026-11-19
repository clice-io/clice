int §(start)start(struct Engine*) { return 1; }
int start(struct Motor*) { return 2; }
template <typename T> int run(T* t) { return start(t); }
int use() { return run((Engine*)nullptr); }
