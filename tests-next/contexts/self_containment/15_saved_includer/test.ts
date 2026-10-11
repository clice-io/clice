import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The header is open while its includer changes what it defines first and
// saves.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("ruler.h")).toEqual([]);
    const scale = t.at("ruler.h", "return |SCALE");
    await t.show("before", s.hover(scale));
    s.edit("main.cpp", { replace: "#define SCALE 3", with: "#define SCALE 4" });
    await t.show("edited", s.hover(scale));
    s.save("main.cpp");
    // Until the header compiles again, its hover may come from the index.
    await s.sync();
    expect(await s.errors("ruler.h")).toEqual([]);
    await t.show("saved", s.hover(scale));
});
