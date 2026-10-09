// A variadic forwarder whose body also makes a call and a construction with
// fewer arguments than its pack: neither can carry the pack, the call that
// does still resolves, and the forwarded argument reads as a write.

struct Tag {
    Tag(int code);
};

void note(int code);
void fill(int& a, int& b, int& c);

template <class... Args>
void relay(Args&... args) {
    note(0);
    Tag tag(1);
    fill(args...);
}

int §(first)first;
int second;
int third;

void use() {
    relay(first, second, third);
}
