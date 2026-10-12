int resize(int width, int height, bool keep_ratio);

int frame() {
    §(calls)⟦int small = resize(320, 240, true);
    int large = resize(1920, 1080, false);⟧
    return resize(small, large, true);
}
