import * as vscode from "vscode";
import {
    GenericNotificationHandler,
    LanguageClient,
    StateChangeEvent,
} from "vscode-languageclient/node";

/// A start() rejection is terminal for a LanguageClient instance: it
/// caches the rejected start promise forever, and stop() refuses to run
/// without a live connection (verified against vscode-languageclient
/// 9.0.1). Recovery therefore needs a fresh instance, so features bind
/// to this stable handle instead of the client; renew() swaps the
/// instance underneath and re-points state events and notification
/// handlers at the replacement.
export class ClientHandle {
    private stateEmitter = new vscode.EventEmitter<StateChangeEvent>();
    private notifications: [string, GenericNotificationHandler][] = [];
    private subscription: vscode.Disposable;
    private client: LanguageClient;

    /// Fires state changes of every client generation.
    readonly onDidChangeState = this.stateEmitter.event;

    constructor(private create: () => LanguageClient) {
        this.client = create();
        this.subscription = this.pipeStateChanges();
    }

    get current(): LanguageClient {
        return this.client;
    }

    isRunning(): boolean {
        return this.client.isRunning();
    }

    sendRequest<R>(method: string, param: unknown): Promise<R> {
        return this.client.sendRequest<R>(method, param);
    }

    onNotification(method: string, handler: GenericNotificationHandler) {
        this.notifications.push([method, handler]);
        this.client.onNotification(method, handler);
    }

    renew() {
        this.subscription.dispose();
        // dispose() rejects when the client never reached Running; the
        // instance is abandoned either way.
        void this.client.dispose().catch(() => undefined);
        this.client = this.create();
        this.subscription = this.pipeStateChanges();
        for (const [method, handler] of this.notifications) {
            this.client.onNotification(method, handler);
        }
    }

    private pipeStateChanges(): vscode.Disposable {
        return this.client.onDidChangeState((event) => {
            this.stateEmitter.fire(event);
        });
    }
}
