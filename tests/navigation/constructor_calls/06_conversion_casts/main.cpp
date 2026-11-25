struct Target {
    int value;
};

struct Source {
    operator Target() const;
};

Target convert(Source source) {
    return §(conversion)Target§(conversion_paren)(source);
}
