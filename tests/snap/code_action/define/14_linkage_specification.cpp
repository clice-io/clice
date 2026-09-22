/// # Declarations under a linkage specification
///
/// - status: supported
///
/// Functions declared with C linkage are defined like any other, inside the linkage block or after a single-declaration form

extern "C" int §(single)c_entry(int argc);

extern "C" {
int §(block)c_helper(int x);
}
