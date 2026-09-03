/// # Member initializer lists — initialized fields highlighted as fields
///
/// - status: supported
/// - issues: clangd#122

struct Widget {
    int width;
    int height;

    Widget(int w, int h) : §width(§w), §height(h) {}
};
