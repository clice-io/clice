struct Holder {
    int §(field_def)field;
    static int §(shared_decl)shared;
};

enum class State { §(Idle_def)Idle };

int §(global_value_def)global_value = 1;

void touch(int §(param_def)param) {
    int §(local_def)local = §(param_use)param + §(global_value_use)global_value;
    Holder h;
    h.§(field_use)field = §(local_use)local;
    Holder::§(shared_use)shared = h.§(field_use2)field;
    State state = State::§(Idle_use)Idle;
}
