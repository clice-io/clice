/// Generated inputs, for a case whose claim is about quantity: many units,
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
