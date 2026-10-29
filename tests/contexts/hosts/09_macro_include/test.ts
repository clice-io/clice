import { answers } from "@clice/tools/testing";

// main.cpp includes widget.h through a macro the scan cannot read: only the
// index names main.cpp among widget.h's includers. Both clients read it and
// compile the header under main.cpp's command, not the one other.cpp would
// lend.
answers({ indexing: true }, async (a) => {
    await a.show("make", a.hover(a.marker("make")));
});
