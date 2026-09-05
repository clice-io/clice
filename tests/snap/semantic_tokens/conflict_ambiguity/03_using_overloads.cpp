/// # Same-kind overload sets
///
/// - status: supported
///
/// A name naming only functions is no conflict

namespace ops {
void apply();
void apply(int level);
}

using ops::§apply;

void run() {
    §apply();
    §apply(1);
}
