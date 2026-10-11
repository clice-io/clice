#define §(macro_def_site)DECLARE_HOOK(name) int name(int value)

§(macro_name_use)DECLARE_HOOK(§(macro_decl)notify);

DECLARE_HOOK(§(macro_def)notify) {
    return value + 1;
}

int trigger(int value) {
    return §(plain_use)notify(value);
}
