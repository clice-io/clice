/// # Lambdas
///
/// - status: supported
///
/// A lambda's capture list, parameter list and body are steps of their own on
/// the way to the whole lambda

int apply(int seed) {
    auto add = [§(capture)seed](int §(param)step) {
        return seed + §(body)step;
    };
    return add(1);
}
