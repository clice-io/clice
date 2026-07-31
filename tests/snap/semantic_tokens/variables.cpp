/// # Declarations & References
///
/// ## Variables — globals, locals, parameters, fields and enum members
///
/// - status: supported
/// - order: 4

struct Holder {
    int §field;
    static int §shared;
};

enum class State { §Idle };

int §global_value = 1;

void touch(int §param) {
    int §local = §param + §global_value;
    Holder h;
    h.§field = §local;
    Holder::§shared = h.§field;
    State state = State::§Idle;
}
