// Long member lists stop at the default limit of 20 entries, nested members
// included; huge initializers are left out of the summary.

namespace long_record {
struct Registers {
    int r00, r01, r02, r03, r04, r05, r06, r07, r08, r09;
    int r10, r11, r12, r13, r14, r15, r16, r17, r18, r19;
    int r20;
};
§(long_record)Registers registers;
}

namespace exact_record {
struct Registers {
    int r00, r01, r02, r03, r04, r05, r06, r07, r08, r09;
    int r10, r11, r12, r13, r14, r15, r16, r17, r18, r19;
};
§(exact_record)Registers registers;
}

namespace long_enum {
enum Opcode {
    Op00, Op01, Op02, Op03, Op04, Op05, Op06, Op07, Op08, Op09,
    Op10, Op11, Op12, Op13, Op14, Op15, Op16, Op17, Op18, Op19,
    Op20, Op21,
};
§(long_enum)Opcode opcode;
}

namespace nested_cut {
struct Packet {
    int r00, r01, r02, r03, r04, r05, r06, r07, r08, r09;
    int r10, r11, r12, r13, r14, r15, r16, r17;
    union {
        int word;
        short half;
        char byte;
    };
    int trailer;
};
§(nested_cut)Packet packet;
}

#define DUPLICATE_FOUR(x) x, x, x, x
#define DUPLICATE_256(x) DUPLICATE_FOUR(DUPLICATE_FOUR(DUPLICATE_FOUR(DUPLICATE_FOUR(x))))
#define SUM_FOUR(x) x + x + x + x
#define SUM_256(x) SUM_FOUR(SUM_FOUR(SUM_FOUR(SUM_FOUR(x))))

namespace huge_initializers {
struct Tables {
    int large_field[256] = {DUPLICATE_256(0)};
    inline static int large_static[256] = {DUPLICATE_256(0)};
    int small_field = 1;
    inline static int small_static = 2;
};
§(huge_record_initializers)Tables tables;

enum Sum { Small = 1, Large = SUM_256(1) };
§(huge_enumerator)Sum sum;
}
