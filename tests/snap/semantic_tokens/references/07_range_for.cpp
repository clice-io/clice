/// # Range-based for
///
/// - status: supported
///
/// Range-for variables keep variable tokens at definitions and uses

struct List {
    int* begin();
    int* end();
};

void iterate(List items) {
    for (auto& §item : §items) {
        §item = 0;
    }
}
