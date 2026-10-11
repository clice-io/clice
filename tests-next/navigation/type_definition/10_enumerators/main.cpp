// An enumerator's type is its enum, scoped or not.

enum class State { Idle, Ready };

enum Mode { Fast, Safe };

int code(State state) {
    return state == State::§(scoped)Ready ? §(unscoped)Fast : Safe;
}
