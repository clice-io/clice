/// # Labels depending on templates
///
/// - status: supported
///
/// A switch with a label depending on template parameters offers no action, since only an instantiation knows which enumerators it covers
///
/// A switch in a template whose labels do not depend on its parameters is
/// completed as anywhere else.

enum class Mode { Read, Write, Append };

template <Mode M>
bool matches(Mode mode) {
    §(value)switch (mode) {
    case M:
        return true;
    }
    return false;
}

template <class T>
int classify(Mode mode) {
    §(expression)switch (mode) {
    case static_cast<Mode>(sizeof(T) - 1):
        return 1;
    }
    return 0;
}

template <class T>
int writable(Mode mode) {
    §(independent)switch (mode) {
    case Mode::Read:
        return 0;
    }
    return 1;
}
