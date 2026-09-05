/// # Range-based for
///
/// - status: supported
///
/// The loop variable at definition and use

struct List {
    int* begin();
    int* end();
};

void iterate(List items) {
    for (auto& §item : §items) {
        §item = 0;
    }
}
