/// # Member initializer lists
///
/// - status: supported
/// - issues: clangd#122
///
/// Member initializer lists highlight initialized names as fields

struct Widget {
    int width;
    int height;

    Widget(int w, int h) : §width(§w), §height(h) {}
};
