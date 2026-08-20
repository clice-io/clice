/// # Designator Hints
///
/// ## Anonymous members — unnamed unions and structs vanish from the designator path
///
/// - status: supported
/// - order: 3

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
