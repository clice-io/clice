use zed_extension_api::{
    self as zed,
    http_client::{HttpMethod, HttpRequest},
    serde_json,
    settings::LspSettings,
    Architecture, DownloadedFileType, LanguageServerId, LanguageServerInstallationStatus, Os,
    Result, Worktree,
};

/// The language server id. This is the `[language_servers.<id>]` key in
/// `extension.toml`, the name users write under `lsp` in `settings.json`, and
/// the name Zed shows in its language server UI.
const SERVER_NAME: &str = "clice";

const REPOSITORY: &str = "clice-io/clice";

/// The default channel.
const STABLE_CHANNEL: &str = "stable";
const PRE_RELEASE_CHANNEL: &str = "pre-release";

/// Archives are unpacked here and renamed to `clice-<version>` once complete,
/// so a version directory never holds a partial installation.
const DOWNLOAD_DIR: &str = "download";

struct CliceExtension {
    cached_binary_path: Option<String>,
}

/// What a release ships for the current platform.
struct Package {
    /// The asset name after `clice-<version>.`.
    asset_suffix: &'static str,
    file_type: DownloadedFileType,
    /// The server executable, relative to the unpacked archive.
    binary: &'static str,
}

impl Package {
    fn current() -> Result<Self> {
        let (os, arch) = zed::current_platform();
        let asset_suffix = match (os, arch) {
            (Os::Linux, Architecture::X8664) => "x86_64-unknown-linux-gnu.tar.gz",
            (Os::Linux, Architecture::Aarch64) => "aarch64-unknown-linux-gnu.tar.gz",
            (Os::Mac, Architecture::X8664) => "x86_64-apple-darwin.tar.gz",
            (Os::Mac, Architecture::Aarch64) => "aarch64-apple-darwin.tar.gz",
            (Os::Windows, Architecture::X8664) => "x86_64-w64-mingw32.zip",
            (Os::Windows, Architecture::Aarch64) => "aarch64-w64-mingw32.zip",
            (os, arch) => return Err(format!("clice has no build for {os:?} on {arch:?}")),
        };

        // Archives carry a top-level `clice/` directory holding `bin/`, `lib/clang`
        // and `clice.toml`; the server resolves its runtime files relative to the
        // executable, so the whole tree has to stay together.
        Ok(match os {
            Os::Windows => Self {
                asset_suffix,
                file_type: DownloadedFileType::Zip,
                binary: "clice/bin/clice.exe",
            },
            _ => Self {
                asset_suffix,
                file_type: DownloadedFileType::GzipTar,
                binary: "clice/bin/clice",
            },
        })
    }
}

fn is_file(path: &str) -> bool {
    std::fs::metadata(path).is_ok_and(|stat| stat.is_file())
}

/// Whether the `lsp.clice.settings.release_channel` value selects
/// pre-releases.
///
/// This lives in `settings` rather than `initialization_options` because the
/// extension never forwards `settings` to the server, so it can hold
/// extension-private values.
///
/// An absent setting means [`STABLE_CHANNEL`]. Any other value is rejected:
/// falling back to a channel would let a typo silently pick a different one
/// than the user asked for.
fn selects_pre_release(channel: Option<&serde_json::Value>) -> Result<bool> {
    let Some(channel) = channel else {
        return Ok(false);
    };
    match channel.as_str() {
        Some(PRE_RELEASE_CHANNEL) => Ok(true),
        Some(STABLE_CHANNEL) => Ok(false),
        _ => Err(format!(
            "unknown lsp.clice.settings.release_channel {channel}; \
             expected {STABLE_CHANNEL:?} or {PRE_RELEASE_CHANNEL:?}"
        )),
    }
}

/// The version and download URL of the release to install, from GitHub's
/// release list (newest first): the newest one of the channel that ships
/// `asset_suffix`. The stable channel takes the newest pre-release while no
/// stable release ships it, so it works before the first stable release and
/// follows stable releases once they exist.
fn select_release(
    releases: &serde_json::Value,
    pre_release: bool,
    asset_suffix: &str,
) -> Option<(String, String)> {
    let newest = |prerelease: bool| {
        releases
            .as_array()?
            .iter()
            .filter(|release| release["prerelease"].as_bool() == Some(prerelease))
            .find_map(|release| {
                // Tags are `v<version>`; asset names carry the bare version.
                let version = release["tag_name"].as_str()?.trim_start_matches('v');
                let name = format!("clice-{version}.{asset_suffix}");
                let asset = release["assets"]
                    .as_array()?
                    .iter()
                    .find(|asset| asset["name"] == name.as_str() && asset["state"] == "uploaded")?;
                let url = asset["browser_download_url"].as_str()?;
                Some((version.to_owned(), url.to_owned()))
            })
    };

    if pre_release {
        newest(true)
    } else {
        newest(false).or_else(|| newest(true))
    }
}

/// `zed::latest_github_release` only offers the newest release of a channel,
/// whose archives are uploaded platform by platform after it is published,
/// and scans only the first page of releases, which a month of nightlies
/// fills.
fn latest_release(pre_release: bool, package: &Package) -> Result<(String, String)> {
    let response = HttpRequest::builder()
        .method(HttpMethod::Get)
        .url(format!(
            "https://api.github.com/repos/{REPOSITORY}/releases?per_page=100"
        ))
        .build()?
        .fetch()
        .map_err(|error| format!("failed to list clice releases: {error}"))?;
    let releases: serde_json::Value =
        serde_json::from_slice(&response.body).map_err(|error| error.to_string())?;

    select_release(&releases, pre_release, package.asset_suffix)
        .ok_or_else(|| format!("no clice release ships a {} archive", package.asset_suffix))
}

/// The `clice-<version>` directories in the work directory.
fn version_dirs() -> impl Iterator<Item = String> {
    std::fs::read_dir(".")
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|entry| entry.file_name().into_string().ok())
        .filter(|name| name.starts_with("clice-"))
}

/// Installs the newest release of the selected channel. Updating is best
/// effort: when it fails (offline, rate limited), the installation an earlier
/// session left behind keeps serving, the highest version if a removal left
/// several.
fn install(language_server_id: &LanguageServerId, worktree: &Worktree) -> Result<String> {
    let settings = LspSettings::for_worktree(SERVER_NAME, worktree)
        .ok()
        .and_then(|settings| settings.settings);
    let pre_release = selects_pre_release(
        settings
            .as_ref()
            .and_then(|settings| settings.get("release_channel")),
    )?;
    let package = Package::current()?;

    zed::set_language_server_installation_status(
        language_server_id,
        &LanguageServerInstallationStatus::CheckingForUpdate,
    );

    let path = install_latest(language_server_id, pre_release, &package).or_else(|error| {
        version_dirs()
            .map(|dir| format!("{dir}/{}", package.binary))
            .filter(|path| is_file(path))
            .max()
            .ok_or(error)
    })?;

    zed::set_language_server_installation_status(
        language_server_id,
        &LanguageServerInstallationStatus::None,
    );
    Ok(path)
}

fn install_latest(
    language_server_id: &LanguageServerId,
    pre_release: bool,
    package: &Package,
) -> Result<String> {
    let (version, url) = latest_release(pre_release, package)?;

    let version_dir = format!("clice-{version}");
    let binary_path = format!("{version_dir}/{}", package.binary);
    if is_file(&binary_path) {
        return Ok(binary_path);
    }

    zed::set_language_server_installation_status(
        language_server_id,
        &LanguageServerInstallationStatus::Downloading,
    );

    std::fs::remove_dir_all(DOWNLOAD_DIR).ok();
    zed::download_file(&url, DOWNLOAD_DIR, package.file_type)
        .map_err(|error| format!("failed to download {url}: {error}"))?;
    let staged_binary = format!("{DOWNLOAD_DIR}/{}", package.binary);
    zed::make_file_executable(&staged_binary)
        .map_err(|error| format!("failed to make {staged_binary} executable: {error}"))?;

    std::fs::remove_dir_all(&version_dir).ok();
    std::fs::rename(DOWNLOAD_DIR, &version_dir)
        .map_err(|error| format!("failed to install {version_dir}: {error}"))?;
    for dir in version_dirs().filter(|dir| *dir != version_dir) {
        std::fs::remove_dir_all(dir).ok();
    }

    Ok(binary_path)
}

impl CliceExtension {
    /// Resolves the server binary, preferring anything the user already has:
    /// `clice` on the worktree's `$PATH`, then a download made earlier in this
    /// session, and only then a new download. Zed itself handles
    /// `lsp.clice.binary.path` and does not ask the extension when it is set.
    fn find_clice_binary(
        &mut self,
        language_server_id: &LanguageServerId,
        worktree: &Worktree,
    ) -> Result<String> {
        if let Some(path) = worktree.which(SERVER_NAME) {
            return Ok(path);
        }

        if let Some(path) = &self.cached_binary_path {
            if is_file(path) {
                return Ok(path.clone());
            }
        }

        let path = install(language_server_id, worktree)?;
        self.cached_binary_path = Some(path.clone());
        Ok(path)
    }
}

impl zed::Extension for CliceExtension {
    fn new() -> Self {
        Self {
            cached_binary_path: None,
        }
    }

    fn language_server_command(
        &mut self,
        language_server_id: &LanguageServerId,
        worktree: &Worktree,
    ) -> Result<zed::Command> {
        Ok(zed::Command {
            command: self.find_clice_binary(language_server_id, worktree)?,
            args: vec!["serve".to_string()],
            env: Default::default(),
        })
    }
}

zed::register_extension!(CliceExtension);

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    const LINUX: &str = "x86_64-unknown-linux-gnu.tar.gz";

    fn release(tag: &str, prerelease: bool, suffixes: &[&str]) -> serde_json::Value {
        let version = tag.trim_start_matches('v');
        let assets: Vec<_> = suffixes
            .iter()
            .map(|suffix| {
                let name = format!("clice-{version}.{suffix}");
                json!({ "name": name, "state": "uploaded", "browser_download_url": format!("https://dl/{name}") })
            })
            .collect();
        json!({ "tag_name": tag, "prerelease": prerelease, "assets": assets })
    }

    fn picked(version: &str, suffix: &str) -> Option<(String, String)> {
        Some((
            version.to_owned(),
            format!("https://dl/clice-{version}.{suffix}"),
        ))
    }

    #[test]
    fn skips_release_still_uploading() {
        let mut uploading = release("v0.3.2026110208", true, &[LINUX]);
        uploading["assets"][0]["state"] = json!("open");
        let releases = json!([
            uploading,
            release("v0.3.2026110108", true, &["x86_64-apple-darwin.tar.gz"]),
            release("v0.3.2026103108", true, &[LINUX]),
            release("v0.3.2026103008", true, &[LINUX]),
        ]);
        assert_eq!(
            select_release(&releases, true, LINUX),
            picked("0.3.2026103108", LINUX)
        );
    }

    #[test]
    fn stable_prefers_stable_release() {
        let releases = json!([
            release("v0.3.2026110108", true, &[LINUX]),
            release("v0.2.0", false, &[LINUX]),
        ]);
        assert_eq!(
            select_release(&releases, false, LINUX),
            picked("0.2.0", LINUX)
        );
        assert_eq!(
            select_release(&releases, true, LINUX),
            picked("0.3.2026110108", LINUX)
        );
    }

    #[test]
    fn stable_falls_back_to_pre_release() {
        let releases = json!([
            release("v0.1.2026100509", true, &[LINUX]),
            release("v0.0.9", false, &["x86_64-w64-mingw32.zip"]),
        ]);
        assert_eq!(
            select_release(&releases, false, LINUX),
            picked("0.1.2026100509", LINUX)
        );
    }

    #[test]
    fn no_release_ships_platform() {
        let releases = json!([release("v0.2.0", false, &[LINUX])]);
        assert_eq!(
            select_release(&releases, false, "x86_64-w64-mingw32.zip"),
            None
        );
        assert_eq!(select_release(&releases, true, LINUX), None);
        assert_eq!(select_release(&json!({}), false, LINUX), None);
    }

    #[test]
    fn parses_release_channel() {
        assert_eq!(selects_pre_release(None), Ok(false));
        assert_eq!(selects_pre_release(Some(&json!("stable"))), Ok(false));
        assert_eq!(selects_pre_release(Some(&json!("pre-release"))), Ok(true));
        assert!(selects_pre_release(Some(&json!("nightly"))).is_err());
        assert!(selects_pre_release(Some(&json!(1))).is_err());
    }
}
