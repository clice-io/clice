/// # Token Correctness
///
/// ## Defaulted and deleted members — special-member names keep their definition tokens
///
/// - status: supported
/// - order: 7

struct Session {
    §Session() = default;
    §Session(const Session&) = delete;
    §~§Session() = default;
};
