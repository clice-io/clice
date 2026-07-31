/// # Conflict & Ambiguity
///
/// ## Same-kind overload sets — a name naming only functions is no conflict
///
/// - status: supported
/// - order: 3

namespace ops {
void apply();
void apply(int level);
}

using ops::§apply;

void run() {
    §apply();
    §apply(1);
}
