#define §(macro_def)LIMIT 10
#define CLAMP(v) ((v) > LIMIT ? LIMIT : (v))
#define RESET_TOTAL total = 0

int §(total)total;

int clamp_total() {
#ifdef LIMIT
    total = CLAMP(§(arg)total);
#endif
    RESET_TOTAL;
    return §(macro_use)LIMIT;
}

#undef LIMIT
