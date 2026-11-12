/// `node tools/testing/editors/prepare.ts <dir>`: the editors' fixtures
/// copied under <dir> for one editor's end-to-end tests (fixtures.ts).

import { prepareFixtures } from "./fixtures.ts";

const [target, ...rest] = process.argv.slice(2);
if (target === undefined || rest.length > 0) {
    console.error("usage: node tools/testing/editors/prepare.ts <dir>");
    process.exit(64);
}
for (const copy of prepareFixtures(target)) {
    console.log(copy);
}
