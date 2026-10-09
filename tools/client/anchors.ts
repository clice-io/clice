/// The anchor check: every code anchor a serve scenario names — the snippet
/// of `at(file, "...")`, what an edit replaces, follows, precedes or removes
/// — must stand exactly once in its file in every sample project the
/// scenario runs on. A case running on several variants needs its anchors
/// in each; the check finds a missing one without starting a server.

import * as fs from "node:fs";
import * as path from "node:path";
import ts from "typescript";
import { PROJECTS_DIR, type Manifest } from "./project.ts";

/// An anchor a scenario names: in a file by path, or by the logical name
/// the projects' manifests map.
interface Use {
    file: { path: string } | { logical: string };
    anchor: string;
    node: ts.Node;
}

const EDIT_ANCHORS = new Set(["replace", "after", "before", "remove"]);

function literal(node: ts.Node | undefined): string | undefined {
    return node !== undefined && ts.isStringLiteralLike(node) ? node.text : undefined;
}

/// The file a scenario names: a literal path, or `s.file("name")`.
function fileOf(node: ts.Node | undefined): Use["file"] | undefined {
    const text = literal(node);
    if (text !== undefined) {
        return { path: text };
    }
    if (
        node !== undefined &&
        ts.isCallExpression(node) &&
        ts.isPropertyAccessExpression(node.expression) &&
        node.expression.name.text === "file"
    ) {
        const logical = literal(node.arguments[0]);
        return logical === undefined ? undefined : { logical };
    }
    return undefined;
}

/// The anchors named inside `root`.
function usesIn(root: ts.Node): Use[] {
    const uses: Use[] = [];
    const visit = (node: ts.Node): void => {
        if (ts.isCallExpression(node)) {
            const callee = node.expression;
            if (ts.isIdentifier(callee) && callee.text === "at") {
                const file = fileOf(node.arguments[0]);
                const anchor = literal(node.arguments[1]);
                if (file !== undefined && anchor !== undefined) {
                    uses.push({ file, anchor, node });
                }
            }
            if (ts.isPropertyAccessExpression(callee) && callee.name.text === "edit") {
                const file = fileOf(node.arguments[0]);
                for (const change of node.arguments.slice(1)) {
                    if (file === undefined || !ts.isObjectLiteralExpression(change)) {
                        continue;
                    }
                    for (const property of change.properties) {
                        if (
                            !ts.isPropertyAssignment(property) ||
                            !ts.isIdentifier(property.name) ||
                            !EDIT_ANCHORS.has(property.name.text)
                        ) {
                            continue;
                        }
                        const anchor = literal(property.initializer);
                        if (anchor !== undefined) {
                            uses.push({ file, anchor, node: property });
                        }
                    }
                }
            }
        }
        ts.forEachChild(node, visit);
    };
    visit(root);
    return uses;
}

/// The projects a `serve(...)` or `serve.each(...)` call names; undefined
/// for another call. `serve.each` takes an array literal or the keys of an
/// object literal declared in the file.
function servedProjects(node: ts.Node, source: ts.SourceFile): string[] | undefined {
    if (!ts.isCallExpression(node)) {
        return undefined;
    }
    const callee = node.expression;
    if (ts.isIdentifier(callee) && callee.text === "serve") {
        const project = literal(node.arguments[0]);
        return project === undefined ? undefined : [project];
    }
    if (
        !ts.isPropertyAccessExpression(callee) ||
        !ts.isIdentifier(callee.expression) ||
        callee.expression.text !== "serve" ||
        callee.name.text !== "each"
    ) {
        return undefined;
    }
    const list = node.arguments[0];
    if (list !== undefined && ts.isArrayLiteralExpression(list)) {
        return list.elements.flatMap((element) => literal(element) ?? []);
    }
    // Object.keys(TABLE)
    if (list !== undefined && ts.isCallExpression(list)) {
        const table = list.arguments[0];
        if (table !== undefined && ts.isIdentifier(table)) {
            const declaration = findConst(source, table.text);
            if (declaration !== undefined && ts.isObjectLiteralExpression(declaration)) {
                return declaration.properties.flatMap((property) =>
                    property.name !== undefined && ts.isStringLiteral(property.name)
                        ? [property.name.text]
                        : [],
                );
            }
        }
    }
    return [];
}

/// The initializer of the file's `const name = ...`, `as const` unwrapped.
function findConst(source: ts.SourceFile, name: string): ts.Expression | undefined {
    for (const statement of source.statements) {
        if (!ts.isVariableStatement(statement)) {
            continue;
        }
        for (const declaration of statement.declarationList.declarations) {
            if (ts.isIdentifier(declaration.name) && declaration.name.text === name) {
                let initializer = declaration.initializer;
                while (initializer !== undefined && ts.isAsExpression(initializer)) {
                    initializer = initializer.expression;
                }
                return initializer;
            }
        }
    }
    return undefined;
}

/// The projects behind a test call's callee: `serve(...)`, a constant
/// bound to one, either followed by `.for(...)`.
function calleeProjects(
    callee: ts.Expression,
    source: ts.SourceFile,
    bound: Map<string, string[]>,
): string[] | undefined {
    if (
        ts.isCallExpression(callee) &&
        ts.isPropertyAccessExpression(callee.expression) &&
        callee.expression.name.text === "for"
    ) {
        return calleeProjects(callee.expression.expression, source, bound);
    }
    if (ts.isIdentifier(callee)) {
        return bound.get(callee.text);
    }
    return servedProjects(callee, source);
}

function checkFile(testFile: string, projectsDir: string): string[] {
    const source = ts.createSourceFile(
        testFile,
        fs.readFileSync(testFile, "utf8"),
        ts.ScriptTarget.Latest,
        true,
    );
    const bound = new Map<string, string[]>();
    for (const statement of source.statements) {
        if (!ts.isVariableStatement(statement)) {
            continue;
        }
        for (const declaration of statement.declarationList.declarations) {
            const projects =
                declaration.initializer === undefined
                    ? undefined
                    : servedProjects(declaration.initializer, source);
            if (projects !== undefined && ts.isIdentifier(declaration.name)) {
                bound.set(declaration.name.text, projects);
            }
        }
    }

    // Each case body with the projects it runs on, and so the table of
    // cases a `.for` names; anchors outside both run on all the file's
    // projects.
    const bodies: { body: ts.Node; projects: string[] }[] = [];
    const visit = (node: ts.Node): void => {
        if (ts.isCallExpression(node)) {
            const projects = calleeProjects(node.expression, source, bound);
            const body = node.arguments.at(-1);
            if (projects !== undefined && body !== undefined && ts.isFunctionLike(body)) {
                bodies.push({ body, projects });
                const table = ts.isCallExpression(node.expression)
                    ? node.expression.arguments[0]
                    : undefined;
                const cases =
                    table !== undefined && ts.isIdentifier(table)
                        ? findConst(source, table.text)
                        : undefined;
                if (cases !== undefined) {
                    bodies.push({ body: cases, projects });
                }
            }
        }
        ts.forEachChild(node, visit);
    };
    visit(source);
    const everywhere = [...new Set(bodies.flatMap(({ projects }) => projects))];
    const inBodies = new Set<ts.Node>();
    const scoped = bodies.flatMap(({ body, projects }) =>
        usesIn(body).map((use) => {
            inBodies.add(use.node);
            return { use, projects };
        }),
    );
    const loose = usesIn(source)
        .filter((use) => !inBodies.has(use.node))
        .map((use) => ({ use, projects: everywhere }));

    const problems: string[] = [];
    for (const { use, projects } of [...scoped, ...loose]) {
        const line = source.getLineAndCharacterOfPosition(use.node.getStart()).line + 1;
        const where = `${path.basename(testFile)}:${line}`;
        for (const project of projects) {
            const problem = checkUse(use, path.join(projectsDir, project));
            if (problem !== undefined) {
                problems.push(`${where}: ${project}: ${problem}`);
            }
        }
    }
    return problems;
}

function checkUse(use: Use, projectDir: string): string | undefined {
    let file: string;
    if ("logical" in use.file) {
        const manifest = JSON.parse(
            fs.readFileSync(path.join(projectDir, "project.json"), "utf8"),
        ) as Manifest;
        const mapped = manifest.files?.[use.file.logical];
        if (mapped === undefined) {
            return `no file is named ${JSON.stringify(use.file.logical)}`;
        }
        file = mapped;
    } else {
        file = use.file.path;
    }
    const full = path.join(projectDir, file);
    if (!fs.existsSync(full)) {
        return `no file ${file}`;
    }
    const snippet = use.anchor.replace("|", "");
    const count = fs.readFileSync(full, "utf8").split(snippet).length - 1;
    return count === 1 ? undefined : `${file} has ${JSON.stringify(snippet)} ${count} times`;
}

/// Every anchor problem of the scenarios in `testFiles`, one line each.
export function checkAnchors(testFiles: readonly string[], projectsDir = PROJECTS_DIR): string[] {
    return testFiles.flatMap((file) => checkFile(file, projectsDir));
}
