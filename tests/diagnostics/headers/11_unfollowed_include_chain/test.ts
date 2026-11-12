import { test } from "@clice/tools/testing";

// widget_user.h needs main.cpp's Widget, but main.cpp includes it by a
// macro after a #line: the include tree counts the line the #line gives,
// and the directive's text names no file, so the context cannot be cut
// from main.cpp. The header compiles without it and says so.
test(async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("widget_user.h", s.diagnostics("widget_user.h"));
});
