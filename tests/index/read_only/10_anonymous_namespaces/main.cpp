namespace {
int §(top)hidden = 1;

struct Impl {
    int §(member)count;
};

static union {
    int §(union_member)raw;
};
}

namespace outer {
namespace {
int §(nested)nested = 2;

namespace inner {
int §(inside)deep = 3;
}
}
}

int sum = hidden + Impl{}.count + raw + outer::nested + outer::inner::deep;
