/** vitest fixtures: the `session` factory spawns clice servers bound to
 * test-data workspaces, with the teardown gates every test must pass
 * (clean shutdown, no anomalies). */

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test as base } from "vitest";
import { assertNoAnomaly } from "./checks.ts";
import { CliceClient } from "./client.ts";
import { DATA_DIR, generateCdb } from "./compile_commands.ts";
import { shutdownClient } from "./lifecycle.ts";

export { expect } from "vitest";

export function cliceExecutable(): string {
  let exe = process.env["CLICE_EXECUTABLE"];
  if (!exe) {
    throw new Error(
      "CLICE_EXECUTABLE is not set; point it at build/<type>/bin/clice",
    );
  }
  if (process.platform === "win32" && !exe.toLowerCase().endsWith(".exe")) {
    const withSuffix = `${exe}.exe`;
    if (fs.existsSync(withSuffix) || !fs.existsSync(exe)) {
      exe = withSuffix;
    }
  }
  if (!fs.existsSync(exe)) {
    throw new Error(`clice executable not found at '${exe}'`);
  }
  return path.resolve(exe);
}

// Tests sharing a data workspace mutate it (.clice cleanup, cmake
// regeneration). Files run in parallel workers, so exclusivity is a
// cross-process lock, not scheduler grouping. Locks live in tmpdir; a
// crashed run's leftover lock is stolen after it goes stale.
const LOCKS_DIR = path.join(os.tmpdir(), "clice-test-workspace-locks");
const LOCK_STALE_MS = 10 * 60 * 1000;

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function acquireWorkspaceLock(name: string): Promise<() => void> {
  fs.mkdirSync(LOCKS_DIR, { recursive: true });
  const lock = path.join(LOCKS_DIR, name.replaceAll(/[\\/]/g, "__"));
  for (;;) {
    try {
      fs.mkdirSync(lock);
      return () => fs.rmdirSync(lock);
    } catch {
      try {
        if (Date.now() - fs.statSync(lock).mtimeMs > LOCK_STALE_MS) {
          fs.rmdirSync(lock);
          continue;
        }
      } catch {
        continue; // Lock vanished between the failed mkdir and the stat.
      }
      await sleep(100);
    }
  }
}

export interface Session {
  client: CliceClient;
  workspace: string;
}

export interface SessionOptions {
  initializationOptions?: Record<string, unknown>;
  /** Anomalies are internal clice bugs — every test session must end
   * without one. Tests that intentionally trigger anomalies opt out here
   * and assert on them explicitly. */
  allowAnomaly?: boolean;
  drainStderr?: boolean;
}

export interface SessionFactory {
  /** Spawn a server initialized on tests/data/<name>. Acquire sessions for
   * multiple workspaces in alphabetical order to avoid lock cycles. */
  (name: string, options?: SessionOptions): Promise<Session>;
  /** Spawn a bare server with no workspace/initialize. */
  bare(options?: SessionOptions & { args?: string[] }): CliceClient;
}

interface OpenedSession {
  client: CliceClient;
  workspace: string | null;
  allowAnomaly: boolean;
}

function prepareWorkspace(workspace: string): void {
  if (fs.existsSync(path.join(workspace, "CMakeLists.txt"))) {
    generateCdb(workspace);
  }
  // Clean up persisted index/cache so each test starts fresh.
  fs.rmSync(path.join(workspace, ".clice"), { recursive: true, force: true });
}

export const test = base.extend<{ session: SessionFactory }>({
  session: async (
    { task }: { task: { result?: { errors?: unknown[] } } },
    use: (factory: SessionFactory) => Promise<void>,
  ) => {
    const opened: OpenedSession[] = [];
    const releases: Array<() => void> = [];

    const factory = async (
      name: string,
      options: SessionOptions = {},
    ): Promise<Session> => {
      const workspace = path.join(DATA_DIR, name);
      releases.push(await acquireWorkspaceLock(name));
      prepareWorkspace(workspace);
      const client = CliceClient.start(cliceExecutable(), {
        drainStderr: options.drainStderr,
      });
      opened.push({
        client,
        workspace,
        allowAnomaly: options.allowAnomaly ?? false,
      });
      await client.initialize(workspace, {
        initializationOptions: options.initializationOptions,
      });
      return { client, workspace };
    };
    factory.bare = (
      options: SessionOptions & { args?: string[] } = {},
    ): CliceClient => {
      const client = CliceClient.start(cliceExecutable(), {
        drainStderr: options.drainStderr,
        args: options.args,
      });
      opened.push({
        client,
        workspace: null,
        allowAnomaly: options.allowAnomaly ?? false,
      });
      return client;
    };

    try {
      await use(factory as SessionFactory);
    } finally {
      const failed = (task.result?.errors?.length ?? 0) > 0;
      const teardownErrors: unknown[] = [];
      for (const session of opened.reverse()) {
        // The anomaly gate must run even when shutdown itself fails — a
        // crashed server is exactly when the anomaly evidence matters most.
        try {
          await shutdownClient(session.client, { verbose: failed });
        } catch (exc) {
          teardownErrors.push(exc);
        } finally {
          try {
            if (!session.allowAnomaly) {
              assertNoAnomaly(session.client, session.workspace);
            }
          } catch (exc) {
            teardownErrors.push(exc);
          }
          if (session.workspace !== null) {
            fs.rmSync(path.join(session.workspace, ".clice"), {
              recursive: true,
              force: true,
            });
          }
        }
      }
      releases.reverse().forEach((release) => release());
      if (teardownErrors.length > 0) {
        throw teardownErrors[0];
      }
    }
  },
});
