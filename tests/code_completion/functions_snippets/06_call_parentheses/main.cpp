namespace ns {
int compute(int x);

template <typename T>
int convert(T value);
}  // namespace ns

using namespace ns;

void bar() {
    int a = compu§(bare);
    int b = compu§(followed)(1);
    int c = conve§(template_arguments)<int>(1);
}

using ns::compu§(using_declaration);
