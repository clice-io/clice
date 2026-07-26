/** vitest globalSetup: runs once in the runner process before any worker,
 * like pytest_configure on the xdist controller — workers would race
 * writing the same compile_commands.json files. */

import { generateTestDataCdbs } from "./compile_commands.ts";

export default function setup(): void {
  generateTestDataCdbs();
}
