/// # Declarations & References
///
/// ## Friend declarations — befriended names resolve to their targets; inline friends define
///
/// - status: supported
/// - order: 24

struct Widget;
void ping();

struct Host {
    friend struct §Widget;
    friend void §ping();
    friend void §inline_friend() {}
};
