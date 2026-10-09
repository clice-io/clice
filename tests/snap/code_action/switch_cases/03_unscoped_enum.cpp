/// # Unscoped enum in a namespace
///
/// - status: supported
///
/// Enumerators of an unscoped enum are qualified with the enum's namespace when the switch lies outside it

namespace io {
enum Mode { Read, Write, Append };
}

void open(io::Mode mode) {
    §(sw)switch (mode) {
    case io::Read:
        break;
    }
}
