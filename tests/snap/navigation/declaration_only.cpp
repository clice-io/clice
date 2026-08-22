/// - verify: server
///
/// Declaration-only symbols: definition falls back to the declarations
/// instead of returning nothing.

extern int §(var_decl)threshold;

int §(fn_decl)probe(int value);

int §(caller)watch(int value) {
    return §(fn_use)probe(value) + §(var_use)threshold;
}
