struct Session {
    §(Session_decl)Session();
    §(tilde_decl)~§(Session_use)Session();
};

Session::§(Session_def)Session() {}

Session::§(tilde_def)~Session() {}

void destroy(Session* session) {
    session->§(tilde_use)~Session();
}
