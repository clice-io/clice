struct Widget {
    Widget();
    Widget(int size);
};

void build() {
    auto* a = new §(paren)Widget(1);
    auto* b = new §(braced)Widget{2};
    auto* c = new §(bare)Widget;
    auto* d = new const §(constant)Widget(3);
    auto* e = new §(array)Widget[4];
}
