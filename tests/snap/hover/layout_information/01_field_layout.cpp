/// # Field layout
///
/// - status: supported
///
/// Size, offset, alignment and padding show on field hover

// snap: The fixed x86-64 target keeps reported bit positions stable.
struct Frame {
    char ki§(plain_field)nd;
    long seque§(padded_field)nce;
};

struct Options {
    unsigned int ena§(bitfield)bled : 1;
    unsigned int la§(bitfield_padding)st : 1;
};
