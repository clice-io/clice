import * as vscode from 'vscode';
import * as fs from 'fs';
import * as path from 'path';
import * as https from 'https';
import * as os from 'os';
// @ts-ignore
import decompress = require('decompress');

interface GitHubRelease {
    tag_name: string;
    assets: {
        name: string;
        browser_download_url: string;
    }[];
}


export async function ensureServerBinary(
    context: vscode.ExtensionContext,
    channel: vscode.OutputChannel
): Promise<string | undefined> {

    const storagePath = context.globalStorageUri.fsPath;
    const platform = os.platform();
    const arch = os.arch();

    channel.appendLine(`[Download] Initializing clice downloader...`);
    channel.appendLine(`[Download] Platform: ${platform}, Arch: ${arch}, Storage: ${storagePath}`);

    let platformKeyword = '';
    let archKeyword = '';
    let binaryName = 'clice';

    if (platform === 'win32') {
        platformKeyword = 'windows';
        archKeyword = 'x64';
        binaryName = 'clice.exe';
    } else if (platform === 'darwin') {
        platformKeyword = 'macos';
        archKeyword = arch;
    } else if (platform === 'linux') {
        platformKeyword = 'linux';
        archKeyword = arch === 'x64' ? 'x86_64' : arch;
    } else {
        const msg = `Unsupported platform: ${platform}`;
        channel.appendLine(`[Download] Error: ${msg}`);
        vscode.window.showErrorMessage(msg);
        return undefined;
    }

    const executablePath = path.join(storagePath, "bin", binaryName);

    if (fs.existsSync(executablePath)) {
        channel.appendLine(`[Download] Found existing binary at: ${executablePath}`);
        // TODO: check tag update
        return executablePath;
    }

    if (!fs.existsSync(storagePath)) {
        channel.appendLine(`[Download] Creating storage directory: ${storagePath}`);
        fs.mkdirSync(storagePath, { recursive: true });
    }

    const statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);

    try {
        statusItem.text = "$(sync~spin) Checking clice updates...";
        statusItem.show();

        channel.appendLine(`[Download] Fetching latest release from GitHub...`);
        const release = await fetchReleaseInfo(channel);
        channel.appendLine(`[Download] Latest tag: ${release.tag_name}`);

        const asset = release.assets.find(a => {
            const name = a.name.toLowerCase();
            return name.includes(platformKeyword) &&
                name.includes(archKeyword) &&
                !name.includes('symbol');
        });

        if (!asset) {
            throw new Error(`No compatible asset found for ${platform}-${archKeyword} in release ${release.tag_name}`);
        }

        channel.appendLine(`[Download] Found asset: ${asset.name}`);
        channel.appendLine(`[Download] Download URL: ${asset.browser_download_url}`);

        const tempArchiveName = asset.name;
        const tempArchivePath = path.join(storagePath, tempArchiveName);

        statusItem.text = `$(cloud-download) Downloading clice...`;
        await downloadFile(asset.browser_download_url, tempArchivePath, channel);

        statusItem.text = `$(file-zip) Extracting...`;
        channel.appendLine(`[Download] Extracting ${tempArchivePath} to ${storagePath}...`);

        await decompress(tempArchivePath, storagePath);
        channel.appendLine(`[Download] Extraction complete.`);

        fs.unlinkSync(tempArchivePath);

        if (!fs.existsSync(executablePath)) {
            throw new Error(`Executable not found at ${executablePath} after extraction.`);
        }

        if (platform !== 'win32') {
            channel.appendLine(`[Download] Setting executable permissions (chmod 755)...`);
            fs.chmodSync(executablePath, '755');
        }

        channel.appendLine(`[Download] Setup successful. Binary ready at: ${executablePath}`);
        vscode.window.showInformationMessage(`Clice language server updated to ${release.tag_name}`);
        return executablePath;

    } catch (error) {
        channel.appendLine(`[Download] CRITICAL ERROR during setup:`);
        if (error instanceof Error) {
            channel.appendLine(`[Download] Message: ${error.message}`);
            if (error.stack) {
                channel.appendLine(`[Download] Stack: ${error.stack}`);
            }
        } else {
            channel.appendLine(`[Download] Unknown error: ${JSON.stringify(error)}`);
        }

        vscode.window.showErrorMessage(`Failed to download clice server. Check "clice" output channel for details.`, "Open Output").then(selection => {
            if (selection === "Open Output") {
                channel.show();
            }
        });

        return undefined;
    } finally {
        statusItem.dispose();
    }
}

function downloadFile(url: string, destPath: string, channel: vscode.OutputChannel, maxRedirects = 5): Promise<void> {
    return new Promise((resolve, reject) => {
        if (maxRedirects <= 0) {
            reject(new Error('Too many redirects'));
            return;
        }

        const file = fs.createWriteStream(destPath);
        channel.appendLine(`[Download] Start downloading to ${destPath}`);

        https.get(url, { headers: { 'User-Agent': 'VSCode-Extension' } }, (response) => {
            if (response.statusCode === 302 || response.statusCode === 301) {
                channel.appendLine(`[Download] Redirecting to ${response.headers.location}`);
                file.close();
                downloadFile(response.headers.location!, destPath, channel, maxRedirects - 1).then(resolve).catch(reject);
                return;
            }
            if (response.statusCode !== 200) {
                reject(new Error(`Download failed with status code ${response.statusCode}`));
                return;
            }

            response.pipe(file);
            file.on('finish', () => {
                file.close();
                channel.appendLine(`[Download] Download finished.`);
                resolve();
            });
        }).on('error', (err) => {
            file.close();
            fs.unlink(destPath, () => { });
            reject(err);
        });
    });
}

async function fetchReleaseInfo(channel: vscode.OutputChannel): Promise<GitHubRelease> {
    try {
        channel.appendLine('[Download] Attempting to fetch latest stable release...');
        const release = await fetchJson<GitHubRelease>('/repos/clice-io/clice/releases/latest');
        channel.appendLine(`[Download] Found stable release: ${release.tag_name}`);
        return release;
    } catch (error: any) {
        if (error.message && error.message.includes('404')) {
            channel.appendLine('[Download] Latest stable release not found (404). Checking for pre-releases...');

            const releases = await fetchJson<GitHubRelease[]>('/repos/clice-io/clice/releases?per_page=1');

            if (Array.isArray(releases) && releases.length > 0) {
                const latestPre = releases[0];
                channel.appendLine(`[Download] Found pre-release: ${latestPre.tag_name}`);
                return latestPre;
            } else {
                throw new Error('No releases found in repository.');
            }
        }
        throw error;
    }
}

function fetchJson<T>(apiPath: string): Promise<T> {
    return new Promise((resolve, reject) => {
        const options = {
            hostname: 'api.github.com',
            path: apiPath,
            headers: { 'User-Agent': 'VSCode-Extension' }
        };

        https.get(options, (res) => {
            let data = '';
            if (res.statusCode && (res.statusCode < 200 || res.statusCode >= 300)) {
                res.resume();
                reject(new Error(`GitHub API returned ${res.statusCode} for ${apiPath}`));
                return;
            }

            res.on('data', (chunk) => data += chunk);
            res.on('end', () => {
                try {
                    resolve(JSON.parse(data));
                } catch (e) {
                    reject(new Error(`Failed to parse GitHub API response: ${e}`));
                }
            });
        }).on('error', reject);
    });
}
