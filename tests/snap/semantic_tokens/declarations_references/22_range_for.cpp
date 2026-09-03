/// # Range-based for — the loop variable at definition and use
///
/// - status: supported

struct List {
    int* begin();
    int* end();
};

void iterate(List items) {
    for (auto& §item : §items) {
        §item = 0;
    }
}
