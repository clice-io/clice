/// # Aggregate initialization — navigate to the struct definition
///
/// - status: supported
/// - verify: server
///
/// An aggregate has no constructor, so go-to-definition on its initializer
/// brace reaches the aggregate's definition.

struct Point {
    int x;
    int y;
};

void use() {
    auto p = Point§(agg_brace){1, 2};
}
