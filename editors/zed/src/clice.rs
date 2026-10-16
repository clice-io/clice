use zed_extension_api::{
    self as zed, serde_json, settings::LspSettings, Architecture, DownloadedFileType,
    GithubRelease, GithubReleaseOptions, LanguageServerId, LanguageServerInstallationStatus, Os,
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

/// The newest release of the channel; `latest(pre_release)` is the newest
/// release that is, or is not, a pre-release. The stable channel takes the
/// newest pre-release while there is no stable release, so it works before the
/// first stable release and follows stable releases once they exist.
fn latest_release(
    pre_release: bool,
    latest: impl Fn(bool) -> Result<GithubRelease>,
) -> Result<GithubRelease> {
    if pre_release {
        latest(true)
    } else {
        latest(false).or_else(|_| latest(true))
    }
}

/// The version and download URL of `release`'s archive for this platform.
fn release_asset(release: &GithubRelease, asset_suffix: &str) -> Result<(String, String)> {
    // Tags are `v<version>`; asset names carry the bare version.
    let version = release.version.trim_start_matches('v');
    let name = format!("clice-{version}.{asset_suffix}");
    let asset = release
        .assets
        .iter()
        .find(|asset| asset.name == name)
        .ok_or_else(|| format!("clice {version} ships no {name}"))?;
    Ok((version.to_owned(), asset.download_url.clone()))
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
    // `zed::latest_github_release` reads only the first page of releases and
    // takes the newest one with any asset. That suffices because the release
    // workflows publish a release only once every platform archive is
    // attached, and nightly.yml prunes nightlies so that the newest stable
    // release stays on the first page.
    let release = latest_release(pre_release, |pre_release| {
        zed::latest_github_release(
            REPOSITORY,
            GithubReleaseOptions {
                require_assets: true,
                pre_release,
            },
        )
    })
    .map_err(|error| format!("failed to find a clice release: {error}"))?;
    let (version, url) = release_asset(&release, package.asset_suffix)?;

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
    use zed_extension_api::GithubReleaseAsset;

    const LINUX: &str = "x86_64-unknown-linux-gnu.tar.gz";

    fn release(tag: &str, suffixes: &[&str]) -> GithubRelease {
        let version = tag.trim_start_matches('v');
        GithubRelease {
            version: tag.to_owned(),
            assets: suffixes
                .iter()
                .map(|suffix| {
                    let name = format!("clice-{version}.{suffix}");
                    GithubReleaseAsset {
                        download_url: format!("https://dl/{name}"),
                        name,
                    }
                })
                .collect(),
        }
    }

    /// `latest` over a release list with only these newest releases.
    fn newest(
        stable: Option<&'static str>,
        pre_release: Option<&'static str>,
    ) -> impl Fn(bool) -> Result<GithubRelease> {
        move |wants_pre_release| {
            let tag = if wants_pre_release {
                pre_release
            } else {
                stable
            };
            tag.map(|tag| release(tag, &[LINUX]))
                .ok_or_else(|| "finding a prerelease".to_owned())
        }
    }

    fn version(release: Result<GithubRelease>) -> Result<String> {
        release.map(|release| release.version)
    }

    #[test]
    fn stable_prefers_stable_release() {
        let latest = newest(Some("v0.2.0"), Some("v0.3.2026110108"));
        assert_eq!(version(latest_release(false, &latest)), Ok("v0.2.0".into()));
        assert_eq!(
            version(latest_release(true, &latest)),
            Ok("v0.3.2026110108".into())
        );
    }

    #[test]
    fn stable_falls_back_to_pre_release() {
        let latest = newest(None, Some("v0.1.2026100509"));
        assert_eq!(
            version(latest_release(false, &latest)),
            Ok("v0.1.2026100509".into())
        );
    }

    #[test]
    fn pre_release_never_takes_stable() {
        assert!(latest_release(true, newest(Some("v0.2.0"), None)).is_err());
        assert!(latest_release(false, newest(None, None)).is_err());
    }

    #[test]
    fn picks_platform_asset() {
        let release = release(
            "v0.1.2026100509",
            &[
                "x86_64-apple-darwin.tar.gz",
                LINUX,
                "x86_64-w64-mingw32.zip",
            ],
        );
        assert_eq!(
            release_asset(&release, LINUX),
            Ok((
                "0.1.2026100509".into(),
                format!("https://dl/clice-0.1.2026100509.{LINUX}")
            ))
        );
        assert!(release_asset(&release, "aarch64-w64-mingw32.zip").is_err());
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
