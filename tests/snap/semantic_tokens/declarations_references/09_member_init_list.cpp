/// # Member initializer lists
///
/// - status: supported
/// - issues: clangd#122
///
/// Initialized fields highlighted as fields

struct Widget {
    int width;
    int height;

    Widget(int w, int h) : §width(§w), §height(h) {}
};
