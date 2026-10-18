import { answers } from "@clice/tools/testing";

// Each class that could name the library's own one reads it among its own
// template arguments: naming it through them must not recurse.
answers((a) => a.show("main.cpp", a.inlayHints("main.cpp")));
