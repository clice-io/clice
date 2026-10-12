// A name naming only functions is no conflict.
namespace ops {
void apply();
void apply(int level);
}

using ops::§(apply_use)apply;

void run() {
    §(apply_use2)apply();
    §(apply_use3)apply(1);
}
