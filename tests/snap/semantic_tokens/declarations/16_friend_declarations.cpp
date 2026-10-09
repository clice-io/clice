/// # Friend declarations
///
/// - status: supported
///
/// Befriended names resolve to their targets; inline friends define

struct Widget;
void ping();

struct Host {
    friend struct §Widget;
    friend void §ping();
    friend void §inline_friend() {}
};
