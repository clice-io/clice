export module B;
export import A;

export int b() {
    return a() + 1;
}
