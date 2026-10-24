/// The default script: every configuration of a project without a test.ts
/// of its own is regression-tested.

import { regression } from "@clice/tools/ecosystem";

regression();
