/// # Complete switch offers nothing
///
/// - status: supported
///
/// A switch handling every enumerator, or one over a non-enum value, offers no action

enum class Bit { Zero, One };

int flip(Bit bit, int n) {
    §(complete)switch (bit) {
    case Bit::Zero:
        return 1;
    case Bit::One:
        return 0;
    }
    §(integer)switch (n) {
    case 0:
        return 1;
    }
    return 0;
}
