import * as vscode from 'vscode';
import { DocumentUri } from 'vscode-languageclient/node';

let provider: HeaderContextProvider | undefined = undefined;

export type HeaderContext = {
    file: string,
    index: number,
    include: number
};

export type HeaderContextSwitchParams = {
    header: DocumentUri,
    context: HeaderContext,
};

export type IncludeLocation = {
    line: number,
    filename: string
};

export class TreeItem extends vscode.TreeItem {
    children: Array<TreeItem> = []
    context: HeaderContext | undefined = undefined;
};

export class HeaderContextProvider implements vscode.TreeDataProvider<TreeItem> {
    private _onDidChangeTreeData: vscode.EventEmitter<TreeItem | undefined | void> = new vscode.EventEmitter<TreeItem | undefined | void>();
    readonly onDidChangeTreeData: vscode.Event<TreeItem | undefined | void> = this._onDidChangeTreeData.event;

    header: string = ""
    items: Array<TreeItem> = []

    update(contexts: Array<Array<HeaderContext>>) {
        /// Create groups
        this.items = contexts.map((contexts) => {
            let item = new TreeItem("", vscode.TreeItemCollapsibleState.Expanded);
            item.children = contexts.map((context) => {
                const uri = vscode.Uri.file(context.file);
                let item = new TreeItem(uri, vscode.TreeItemCollapsibleState.None);
                item.context = context;
                item.iconPath = vscode.ThemeIcon.File;
                item.contextValue = "header-context";
                return item;
            });
            return item;
        });

        this.refresh();
    }

    refresh(): void {
        this._onDidChangeTreeData.fire();
    }

    getTreeItem(element: TreeItem) {
        return element;
    }

    getChildren(element?: TreeItem) {
        return element ? element.children : this.items;
    }
};



export function registerHeaderContextView() {
    provider = new HeaderContextProvider();
    let treeView = vscode.window.createTreeView("header-contexts", { treeDataProvider: provider });
}
