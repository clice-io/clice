import { createSessionFactory, type SessionFactory } from "@clice/tools/session";

export async function sessionFixture(
    { task }: { task: { result?: { errors?: unknown[] } } },
    use: (factory: SessionFactory) => Promise<void>,
): Promise<void> {
    const handle = createSessionFactory();
    try {
        await use(handle.session);
    } finally {
        await handle.teardown((task.result?.errors?.length ?? 0) > 0);
    }
}
