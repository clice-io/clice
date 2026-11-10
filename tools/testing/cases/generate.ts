/// Generated inputs, for a case whose point is quantity: many units,
/// many symbols, a large file. Every generated name carries its number, so
/// answers about one item never match another.

function numbered(index: number, count: number): string {
    return String(index).padStart(String(Math.max(count - 1, 0)).length, "0");
}

/// `count` independent units, `unit_<n>.cpp`, each defining `int unit_<n>()`.
export function units(count: number): Record<string, string> {
    const files: Record<string, string> = {};
    for (let i = 0; i < count; i++) {
        const n = numbered(i, count);
        files[`unit_${n}.cpp`] = `int unit_${n}() { return ${String(i)}; }\n`;
    }
    return files;
}

/// The names `units(count)` defines, in order.
export function unitSymbols(count: number): string[] {
    return Array.from({ length: count }, (_, i) => `unit_${numbered(i, count)}`);
}

/// Source text defining `count` functions `int function_<n>()`.
export function functions(count: number): string {
    return Array.from({ length: count }, (_, i) => {
        const n = numbered(i, count);
        return `int function_${n}() { return ${String(i)}; }\n`;
    }).join("");
}

/// `text` followed by comment lines until it is at least `bytes` long.
export function padded(text: string, bytes: number): string {
    const line = "// padding\n";
    const missing = Math.max(bytes - Buffer.byteLength(text), 0);
    return text + line.repeat(Math.ceil(missing / line.length));
}

/// `depth` layers of two modules, `layer_<n>_a` and `layer_<n>_b`, each
/// importing both modules of the layer below, down to the module `base`,
/// which the caller writes; `main.cpp` imports the top layer. There are
/// 2^depth import paths from main.cpp to `base`.
export function moduleLayers(depth: number): Record<string, string> {
    const files: Record<string, string> = {};
    let below = ["base"];
    for (let level = 1; level <= depth; level++) {
        const n = numbered(level, depth + 1);
        const names = [`layer_${n}_a`, `layer_${n}_b`];
        for (const name of names) {
            const imports = below.map((module) => `import ${module};\n`).join("");
            files[`${name}.cppm`] = `export module ${name};\n${imports}export int ${name}();\n`;
        }
        below = names;
    }
    files["main.cpp"] = `${below.map((module) => `import ${module};\n`).join("")}int main() {}\n`;
    return files;
}
