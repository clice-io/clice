/// # Declarations & References
///
/// ## Lambda captures — by-copy and by-reference captures reference the captured variable; `this` stays a keyword
///
/// - status: supported
/// - order: 21

struct S {
    int field;

    int compute() {
        int local = 1;
        auto by_copy = [§local, §this] {
            return §local + this->§field;
        };
        auto by_reference = [&§local] {
            return §local;
        };
        return by_copy() + by_reference();
    }
};
