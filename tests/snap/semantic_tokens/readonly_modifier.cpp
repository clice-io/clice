/// # Token Modifiers
///
/// ## Readonly — const and constexpr values, const methods and enum members
///
/// - status: supported
/// - order: 3
///
/// Readonly is currently value-based: a pointer to const counts as
/// readonly even though the pointer itself can change.

enum class Level { §High };

const int §limit = 10;
constexpr int §bound = 4;

struct Gauge {
    int §read() const;
    void §write(int value);
};

void probe(const int& §in, const int* §pointee_const, int* const §self_const) {
    Gauge gauge;
    gauge.§read();
    gauge.§write(§limit);
}
