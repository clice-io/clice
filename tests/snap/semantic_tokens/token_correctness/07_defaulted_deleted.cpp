/// # Defaulted and deleted members — special-member names keep their definition tokens
///
/// - status: supported

struct Session {
    §Session() = default;
    §Session(const Session&) = delete;
    §~§Session() = default;
};
