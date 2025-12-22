import * as vscode from 'vscode';

const rainbowColors = [
    "#56B6C2",
    "#61AFEF",
    "#C678DD",
    "#E06C75",
    "#98C379",
    "#D19A66",
    "#E5C07B"
];

const textEditorDecorationTypes = rainbowColors.map((color) => {
    return vscode.window.createTextEditorDecorationType({
        color: color
    });
});

export function highlightDocument(document: vscode.TextDocument, legend: vscode.SemanticTokensLegend, semanticTokens: vscode.SemanticTokens) {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document !== document) { return; }
    const angleIndex = legend?.tokenTypes.indexOf('angle');
    const leftIndex = legend?.tokenModifiers.indexOf('left');
    const rightIndex = legend?.tokenModifiers.indexOf('right');
    if (leftIndex === undefined || rightIndex === undefined || angleIndex === undefined) { return; }

    const decorations = new Map<number, vscode.Range[]>();
    let level = 0;

    let lastLine = 0;
    let lastStart = 0;

    // [line, startCharacter, length, tokenType, tokenModifiers]
    for (let i = 0; i < semanticTokens.data.length; i += 5) {
        const [lineDelta, startDelta, length, tokenType, tokenModifiers] = semanticTokens.data.slice(i, i + 5);

        lastLine += lineDelta;
        lastStart = lineDelta === 0 ? lastStart + startDelta : startDelta;

        const range = new vscode.Range(lastLine, lastStart, lastLine, lastStart + length);

        if (tokenType === angleIndex) {
            if (tokenModifiers & (1 << rightIndex)) {
                level -= 1;
            }

            if (decorations.has(level % rainbowColors.length)) {
                decorations.get(level % rainbowColors.length)?.push(range);
            } else {
                decorations.set(level % rainbowColors.length, [range]);
            }

            if (tokenModifiers & (1 << leftIndex)) {
                level += 1;

            }
        }
    }

    for (const [level, ranges] of decorations) {
        editor.setDecorations(
            textEditorDecorationTypes[level],
            ranges
        );
    }
}
