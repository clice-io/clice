/// # Cases before the default
///
/// - status: supported
///
/// With a `default` present, the missing cases go right before it and fall through into it, keeping the behavior
///
/// The action is offered from anywhere inside the switch.

enum class Level { Low, Mid, High };

int rank(Level level) {
    switch (level) {
    case Level::Low:
        return §(inside)0;
    default:
        return 1;
    }
}
