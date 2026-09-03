/// # Same-kind overload sets — a name naming only functions is no conflict
///
/// - status: supported

namespace ops {
void apply();
void apply(int level);
}

using ops::§apply;

void run() {
    §apply();
    §apply(1);
}
