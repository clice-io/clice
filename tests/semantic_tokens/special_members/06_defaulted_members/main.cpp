// Special-member names keep their definition tokens.
struct Session {
    §(Session_def)Session() = default;
    §(Session_def2)Session(const Session&) = delete;
    §(tilde)~§(Session_use)Session() = default;
};
