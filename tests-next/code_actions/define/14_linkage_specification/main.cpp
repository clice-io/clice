// Functions declared with C linkage are defined like any other: after a single
// declaration, or inside the linkage block.

extern "C" int §(single)c_entry(int argc);

extern "C" {
int §(block)c_helper(int x);
}
