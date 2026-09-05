/// # Defaulted and deleted members
///
/// - status: supported
///
/// special-member names keep their definition tokens

struct Session {
    §Session() = default;
    §Session(const Session&) = delete;
    §~§Session() = default;
};
