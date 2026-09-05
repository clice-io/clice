/// # Out-of-line member definitions
///
/// - status: supported
///
/// Qualified names keep method kinds and modifiers

struct Gauge {
    int read() const;
    static void reset();
};

int §Gauge::§read() const {
    return 0;
}

void Gauge::§reset() {}
