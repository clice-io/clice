import { test } from "@clice/tools/testing";

// Db::open is defined twice, in db.cpp, db.h's host, and in mock_db.cpp,
// which a test binary links instead. Defining Db::close in the host goes
// after the host's own definitions of Db's members; mock_db.cpp is open, so
// its index answers first.
test("host member defined twice", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: true } } });
    await s.diagnostics("mock_db.cpp");
    await s.diagnostics("db.h");
    await s.indexed();
    await t.show("db.h", s.applyAction(t.marker("close"), "Define 'Db::close' in db.cpp"));
    await t.show("db.cpp text", t.disk.read("db.cpp"));
    await t.show("db.cpp", s.errors("db.cpp"));
});
