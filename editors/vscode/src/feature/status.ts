import * as vscode from "vscode";

const selector: vscode.DocumentSelector = [
    { scheme: "file", language: "c" },
    { scheme: "file", language: "cpp" },
    { scheme: "file", language: "cuda-cpp" },
];

/// A language status item, listed under the `{}` beside the language mode
/// of a C, C++ or CUDA editor while it is shown; the user can pin it to the
/// status bar. VS Code lists every item that exists, so hiding one disposes
/// it.
export class LanguageStatus implements vscode.Disposable {
    private item: vscode.LanguageStatusItem | undefined;

    constructor(
        private readonly id: string,
        private readonly name: string,
    ) {}

    show(text: string, detail: string, command: vscode.Command, warn = false) {
        if (!this.item) {
            this.item = vscode.languages.createLanguageStatusItem(this.id, selector);
            this.item.name = this.name;
        }
        this.item.text = text;
        this.item.detail = detail;
        this.item.command = command;
        this.item.severity = warn
            ? vscode.LanguageStatusSeverity.Warning
            : vscode.LanguageStatusSeverity.Information;
    }

    hide() {
        this.item?.dispose();
        this.item = undefined;
    }

    dispose() {
        this.hide();
    }
}
