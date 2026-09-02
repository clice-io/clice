/// # Friend declarations — befriended names resolve to their targets; inline friends define
///
/// - status: supported

struct Widget;
void ping();

struct Host {
    friend struct §Widget;
    friend void §ping();
    friend void §inline_friend() {}
};
