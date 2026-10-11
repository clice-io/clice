// A variable a lambda captures by default highlights in the body, written
// through a reference capture and read through a copy.

int captures() {
    int §(implicit)implicit = 0;
    auto bump = [&] { implicit += 1; };
    auto peek = [=] { return implicit; };
    bump();
    return peek();
}
