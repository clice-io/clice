int §(global)global_scope;
static int §(in_file)file_scope;

struct Foo {
    int §(in_class)class_scope;

    void bar() {
        int §(in_function)function_scope = 0;
    }
};
