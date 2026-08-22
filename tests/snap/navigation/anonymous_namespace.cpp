/// - verify: server

namespace {

int §(hidden_var)state = 1;

int §(hidden_fn)next() {
    state += 1;
    return state;
}

}

int read_hidden() {
    return §(fn_use)next() + §(var_use)state;
}
