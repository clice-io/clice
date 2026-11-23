import type * as proto from "vscode-languageserver-protocol";
import { test } from "@clice/tools/testing";

// Neovim 0.12 asks for a range of semantic tokens beside the full ones after
// a refresh and, when the range answer comes first, applies the next delta to
// it (neovim#42356): the server offers it no range. Before 0.12 Neovim asks
// for none, from 0.13 it applies them right. The versions are spelled as
// Neovim sends them: a release, a build of the release-0.12 branch, a nightly.
for (const version of ["0.12.6", "0.12.7-dev+g1a2b3c4d5e", "0.13.0-dev+gbd54353246", "0.11.5"]) {
    test(`Neovim ${version}`, async (t) => {
        const s = await t.start({ clientInfo: { name: "Neovim", version } });
        const { range, full } = s.client.initResult?.capabilities
            .semanticTokensProvider as proto.SemanticTokensOptions;
        await t.show("semantic tokens", JSON.stringify({ range, full }, null, 4));
    });
}
