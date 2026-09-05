/// # Field layout
///
/// - status: supported
///
/// Size, offset, alignment and padding show on field hover
///
/// The corpus uses a fixed x86-64 target, so the reported bit positions are stable.

struct Frame {
    char ki§(plain_field)nd;
    long seque§(padded_field)nce;
};

struct Options {
    unsigned int ena§(bitfield)bled : 1;
    unsigned int la§(bitfield_padding)st : 1;
};
