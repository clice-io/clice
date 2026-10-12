import { answers } from "@clice/tools/testing";

// Every marker is asked as typing the character before it asks: an editor
// sends the trigger character it just inserted.
answers(
    { paths: { server: "a trigger character is what an editor sends while typing" } },
    async (a) => {
        for (const m of a.markers()) {
            const text = m.text.slice(0, m.begin);
            await a.show(m.name, a.completion(m, text.at(-1)));
        }
    },
);
