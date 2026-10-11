#define DEFINE_COUNTER int §(body_name)counter_value = 0; int next_value()

#define MAKE_FLAG(name) bool flag_##name = false

§(counter_site)DEFINE_COUNTER {
    return §(counter_use)counter_value + 1;
}

§(flag_site)MAKE_FLAG(verbose);

bool read_flag() {
    return §(flag_use)flag_verbose;
}
