import { answers } from "@clice/tools/testing";

answers((a) => a.show("pos", a.completion(a.marker("pos")), { parts: ["edit"] }));
