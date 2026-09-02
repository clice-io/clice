/// # Anonymous members — unnamed unions and structs vanish from the designator path
///
/// - status: supported

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
