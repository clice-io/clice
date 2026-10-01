/// # Labels depending on templates
///
/// - status: supported
///
/// A switch with a label depending on template parameters offers no action, since only an instantiation knows which enumerators it covers

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
