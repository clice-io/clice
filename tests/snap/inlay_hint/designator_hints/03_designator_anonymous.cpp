/// # Anonymous members
///
/// - status: supported
///
/// Unnamed unions and structs vanish from the designator path

struct State {
    union {
        struct {
            struct {
                int y;
            };
        } x;
    };
};

State s{42};
