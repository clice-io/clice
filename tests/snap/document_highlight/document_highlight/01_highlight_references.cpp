/// # Document reference highlights
///
/// - status: supported
///
/// Every name of the symbol under the cursor in the current file is
/// highlighted, its declarations and definition included

int §(global_decl)total = 0;

int §(function_decl)accumulate(int amount);

int accumulate(int §(param_decl)amount) {
    total = total + amount;
    return §(global_use)total * §(param_use)amount;
}

int twice() {
    return §(function_use)accumulate(1) + accumulate(2);
}
