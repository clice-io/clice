/// The sample projects themselves: every unit of every variant compiles
/// clean and indexes.

import { serve } from "../fixtures.ts";

serve.each(["tiny", "shapes/headers", "shapes/modules"])(
    "every unit compiles clean",
    async ({ s }) => {
        for (const unit of Object.keys(s.manifest.units ?? {})) {
            await s.clean(unit);
        }
        await s.indexed();
    },
);
