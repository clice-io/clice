/// The compatibility matrix. Each scenario names its compiler by absolute
/// path where the system has several, so the database records the
/// toolchain under test rather than whatever PATH finds first.

import type { Scenario } from "@clice/tools/compat/scenario";
import * as compilers from "./scenarios/compilers.ts";
import * as mingw from "./scenarios/mingw.ts";
import * as cross from "./scenarios/cross.ts";
import * as build_systems from "./scenarios/build_systems.ts";
import * as modules from "./scenarios/modules.ts";
import * as offload from "./scenarios/offload.ts";

export const SCENARIOS: Scenario[] = [
    ...compilers.SCENARIOS,
    ...mingw.SCENARIOS,
    ...cross.SCENARIOS,
    ...build_systems.SCENARIOS,
    ...modules.SCENARIOS,
    ...offload.SCENARIOS,
];
