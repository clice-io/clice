/// - verify: server

enum class State {
    §(idle_def)Idle,
    §(ready_def)Ready,
};

int state_code(State state) {
    switch (state) {
    case State::§(idle_case)Idle:
        return 0;
    case State::§(ready_case)Ready:
        return 1;
    }
    return state == State::§(ready_use)Ready ? 2 : 3;
}
