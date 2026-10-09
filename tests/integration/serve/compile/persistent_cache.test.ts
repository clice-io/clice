/// Built PCHs and PCMs outlive the server that built them, and a change
/// made while no server runs reaches them.

import { expect, serve } from "../../fixtures.ts";

serve("shapes/headers")("pch survives server restart", async ({ s }) => {
    await s.clean("app/main.cpp");
    expect((await s.counts()).pch).toBe(1);
    await s.restart();
    await s.clean("app/main.cpp");
    expect((await s.counts()).pch).toBe(0);
});

/// What main.cpp's compile consumes of the shapes library.
const ARTIFACT = { "shapes/headers": "pch", "shapes/modules": "pcm" } as const;

serve.each(Object.keys(ARTIFACT))("offline edit reaches callers", async ({ s }) => {
    const artifact = ARTIFACT[s.project as keyof typeof ARTIFACT];
    await s.clean("app/main.cpp");
    expect((await s.counts())[artifact]).toBeGreaterThan(0);

    await s.offline(() => {
        s.disk.edit(s.file("circle"), { replace: "double area(", with: "double surface(" });
    });
    const errors = await s.errors("app/main.cpp");
    expect(
        s.show(errors.map((error) => ({ uri: s.uri("app/main.cpp"), range: error.range }))),
    ).toBe(
        "app/main.cpp: double total = shapes::area(c) + triangle.measure() + shapes_circle_area(1.0);",
    );
    expect((await s.counts())[artifact], `the ${artifact} is built again`).toBeGreaterThan(0);
});
