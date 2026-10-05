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

/// clice publishes pre-releases only, so that is the default channel.
const PRE_RELEASE_CHANNEL: &str = "pre-release";
const STABLE_CHANNEL: &str = "stable";

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

impl CliceExtension {
    /// Whether `lsp.clice.settings.release_channel` selects pre-releases.
    ///
    /// This lives in `settings` rather than `initialization_options` because the
    /// extension never forwards `settings` to the server, so it can hold
    /// extension-private values.
    ///
    /// An absent setting means [`PRE_RELEASE_CHANNEL`]. Any other value is
    /// rejected: falling back to a channel would let a typo silently pick a
    /// different one than the user asked for.
    fn wants_pre_release(worktree: &Worktree) -> Result<bool> {
        let channel = LspSettings::for_worktree(SERVER_NAME, worktree)
            .ok()
            .and_then(|settings| settings.settings)
            .and_then(|settings| settings.get("release_channel").cloned());

        let Some(channel) = channel else {
            return Ok(true);
        };
        match channel.as_str() {
            Some(PRE_RELEASE_CHANNEL) => Ok(true),
            Some(STABLE_CHANNEL) => Ok(false),
            _ => Err(format!(
                "unknown lsp.clice.settings.release_channel {channel}; \
                 expected {PRE_RELEASE_CHANNEL:?} or {STABLE_CHANNEL:?}"
            )),
        }
    }

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

        let path = Self::install(language_server_id, worktree)?;
        self.cached_binary_path = Some(path.clone());
        Ok(path)
    }

    /// Installs the newest release of the selected channel. Updating is best
    /// effort: when it fails (offline, rate limited), an earlier installation
    /// keeps serving.
    fn install(language_server_id: &LanguageServerId, worktree: &Worktree) -> Result<String> {
        let pre_release = Self::wants_pre_release(worktree)?;
        let package = Package::current()?;

        zed::set_language_server_installation_status(
            language_server_id,
            &LanguageServerInstallationStatus::CheckingForUpdate,
        );

        let path = Self::install_latest(language_server_id, pre_release, &package)
            .or_else(|error| Self::installed_binary(&package).ok_or(error))?;

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
        let (version, url) = Self::latest_release(pre_release, package)?;

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
        zed::make_file_executable(&format!("{DOWNLOAD_DIR}/{}", package.binary))
            .map_err(|error| format!("failed to make {binary_path} executable: {error}"))?;

        std::fs::remove_dir_all(&version_dir).ok();
        std::fs::rename(DOWNLOAD_DIR, &version_dir)
            .map_err(|error| format!("failed to install {version_dir}: {error}"))?;
        Self::remove_other_versions(&version_dir);

        Ok(binary_path)
    }

    /// The version and download URL of the newest release in the channel that
    /// ships this platform's archive.
    ///
    /// `zed::latest_github_release` only offers the newest release, whose
    /// archives are uploaded platform by platform after it is published, and
    /// scans only the first page of releases, which a month of nightlies fills.
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

        releases
            .as_array()
            .into_iter()
            .flatten()
            .filter(|release| release["prerelease"].as_bool() == Some(pre_release))
            .find_map(|release| {
                // Tags are `v<version>`; asset names carry the bare version.
                let version = release["tag_name"].as_str()?.trim_start_matches('v');
                let name = format!("clice-{version}.{}", package.asset_suffix);
                let asset = release["assets"]
                    .as_array()?
                    .iter()
                    .find(|asset| asset["name"] == name.as_str())?;
                let url = asset["browser_download_url"].as_str()?;
                Some((version.to_owned(), url.to_owned()))
            })
            .ok_or_else(|| {
                let channel = if pre_release {
                    PRE_RELEASE_CHANNEL
                } else {
                    STABLE_CHANNEL
                };
                format!(
                    "no clice {channel} release ships a {} archive",
                    package.asset_suffix
                )
            })
    }

    /// The newest installation an earlier session left behind.
    fn installed_binary(package: &Package) -> Option<String> {
        std::fs::read_dir(".")
            .ok()?
            .flatten()
            .filter_map(|entry| entry.file_name().into_string().ok())
            .filter(|name| name.starts_with("clice-"))
            .map(|name| format!("{name}/{}", package.binary))
            .filter(|path| is_file(path))
            .max()
    }

    fn remove_other_versions(current: &str) {
        let Ok(entries) = std::fs::read_dir(".") else {
            return;
        };
        for entry in entries.flatten() {
            let name = entry.file_name();
            let name = name.to_string_lossy();
            if name.starts_with("clice-") && name != current {
                std::fs::remove_dir_all(entry.path()).ok();
            }
        }
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
