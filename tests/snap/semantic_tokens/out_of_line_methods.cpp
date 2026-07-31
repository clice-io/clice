/// # Declarations & References
///
/// ## Out-of-line member definitions — qualified names keep method kinds and modifiers
///
/// - status: supported
/// - order: 18

struct Gauge {
    int read() const;
    static void reset();
};

int §Gauge::§read() const {
    return 0;
}

void Gauge::§reset() {}
