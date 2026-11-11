import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index");
    await t.cli("query definition Shape");
    await t.cli("query references Shape::setX:y:");
    await t.cli("query references Drawable::draw");
});
