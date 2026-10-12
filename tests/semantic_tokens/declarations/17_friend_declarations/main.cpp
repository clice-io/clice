// Befriended names resolve to their targets; inline friends define.
struct Widget;
void ping();

struct Host {
    friend struct §(Widget)Widget;
    friend void §(ping)ping();
    friend void §(inline_friend)inline_friend() {}
};
