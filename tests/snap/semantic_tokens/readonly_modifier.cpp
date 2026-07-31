/// # Token Modifiers
///
/// ## Readonly — const values, const methods and enum members
///
/// - status: supported
/// - order: 3
///
/// Readonly is currently value-based: a pointer to const counts as
/// readonly even though the pointer itself can change.

enum class Level { §High };

const int §limit = 10;

struct Gauge {
    int §read() const;
    void §write(int value);
};

void probe(const int& §bound, const int* §pointee_const, int* const §self_const) {
    Gauge gauge;
    gauge.§read();
    gauge.§write(§limit);
}
