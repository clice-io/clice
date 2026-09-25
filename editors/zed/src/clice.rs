use zed_extension_api::{
    self as zed, Architecture, DownloadedFileType, GithubReleaseOptions, LanguageServerId,
    LanguageServerInstallationStatus, Os, Result, Worktree, settings::LspSettings,
};

/// The language server id. This is the `[language_servers.<id>]` key in
/// `extension.toml`, the name users write under `lsp` in `settings.json`, and
/// the name Zed shows in its language server UI.
const SERVER_NAME: &str = "clice";

const REPOSITORY: &str = "clice-io/clice";

/// clice publishes pre-releases only, so that is the default channel. Selecting
/// `stable` before the first stable release fails with an explicit message
/// rather than silently falling back.
const PRE_RELEASE_CHANNEL: &str = "pre-release";
const STABLE_CHANNEL: &str = "stable";

struct CliceExtension {
    cached_binary_path: Option<String>,
}

impl CliceExtension {
    /// The release channel selected through `lsp.clice.settings.release_channel`.
    ///
    /// This lives in `settings` rather than `initialization_options` because
    /// clice never issues `workspace/configuration`, so `settings` is never
    /// forwarded to the server and can hold extension-private values.
    ///
    /// An absent setting means [`PRE_RELEASE_CHANNEL`]. Any other value is
    /// rejected: falling back to a channel would let a typo silently pick a
    /// different one than the user asked for.
    fn release_channel(worktree: &Worktree) -> Result<String> {
        let configured = LspSettings::for_worktree(SERVER_NAME, worktree)
            .ok()
            .and_then(|settings| settings.settings)
            .and_then(|settings| settings.get("release_channel").cloned())
            .and_then(|channel| channel.as_str().map(str::to_owned));

        match configured.as_deref() {
            None => Ok(PRE_RELEASE_CHANNEL.to_owned()),
            Some(PRE_RELEASE_CHANNEL) => Ok(PRE_RELEASE_CHANNEL.to_owned()),
            Some(STABLE_CHANNEL) => Ok(STABLE_CHANNEL.to_owned()),
            Some(unknown) => Err(format!(
                "unknown lsp.clice.settings.release_channel {unknown:?}; \
                 expected {PRE_RELEASE_CHANNEL:?} or {STABLE_CHANNEL:?}"
            )),
        }
    }

    /// Resolves the server binary, preferring anything the user already has:
    /// an explicit `lsp.clice.binary.path`, then a path resolved earlier in this
    /// session, then `clice` on `$PATH`, and only then a download.
    fn find_clice_binary(
        &mut self,
        language_server_id: &LanguageServerId,
        worktree: &Worktree,
    ) -> Result<String> {
        let configured_path = LspSettings::for_worktree(SERVER_NAME, worktree)
            .ok()
            .and_then(|settings| settings.binary)
            .and_then(|binary| binary.path);

        if let Some(path) = configured_path {
            return Ok(path);
        }

        if let Some(path) = &self.cached_binary_path {
            if std::fs::metadata(path).is_ok_and(|stat| stat.is_file()) {
                return Ok(path.clone());
            }
        }

        if let Some(path) = worktree.which(SERVER_NAME) {
            self.cached_binary_path = Some(path.clone());
            return Ok(path);
        }

        let path = Self::download(language_server_id, worktree)?;
        self.cached_binary_path = Some(path.clone());
        Ok(path)
    }

    fn download(
        language_server_id: &LanguageServerId,
        worktree: &Worktree,
    ) -> Result<String> {
        let channel = Self::release_channel(worktree)?;
        let pre_release = channel != STABLE_CHANNEL;

        zed::set_language_server_installation_status(
            language_server_id,
            &LanguageServerInstallationStatus::CheckingForUpdate,
        );

        // `latest_github_release` matches `pre_release` exactly, so a
        // channel with no matching release is an error rather than an
        // empty result.
        let release = match zed::latest_github_release(
            REPOSITORY,
            GithubReleaseOptions {
                require_assets: true,
                pre_release,
            },
        ) {
            Ok(release) => release,
            Err(error) => {
                let message = match pre_release {
                    true => format!("failed to find a clice pre-release: {error}"),
                    false => format!(
                        "no stable clice release exists yet; set \
                         \"lsp\": {{\"clice\": {{\"settings\": {{\"release_channel\": \
                         \"{PRE_RELEASE_CHANNEL}\"}}}}}} to use a pre-release ({error})"
                    ),
                };
                zed::set_language_server_installation_status(
                    language_server_id,
                    &LanguageServerInstallationStatus::Failed(message.clone()),
                );
                return Err(message);
            }
        };

        let (os, arch) = zed::current_platform();

        let (suffix, file_type, executable) = match (os, arch) {
            (Os::Linux, Architecture::X8664) => (
                "x86_64-unknown-linux-gnu.tar.gz",
                DownloadedFileType::GzipTar,
                "clice",
            ),
            (Os::Linux, Architecture::Aarch64) => (
                "aarch64-unknown-linux-gnu.tar.gz",
                DownloadedFileType::GzipTar,
                "clice",
            ),
            (Os::Mac, Architecture::X8664) => (
                "x86_64-apple-darwin.tar.gz",
                DownloadedFileType::GzipTar,
                "clice",
            ),
            (Os::Mac, Architecture::Aarch64) => (
                "aarch64-apple-darwin.tar.gz",
                DownloadedFileType::GzipTar,
                "clice",
            ),
            (Os::Windows, Architecture::X8664) => (
                "x86_64-pc-windows-msvc.zip",
                DownloadedFileType::Zip,
                "clice.exe",
            ),
            (Os::Windows, Architecture::Aarch64) => (
                "aarch64-pc-windows-msvc.zip",
                DownloadedFileType::Zip,
                "clice.exe",
            ),
            (os, arch) => {
                return Err(format!("clice has no build for {os:?} on {arch:?}"));
            }
        };

        // `release.version` is the tag (`v0.1.2026071902`); asset names drop the
        // leading `v` (`clice-0.1.2026071902.<triple>.tar.gz`).
        let version = release.version.trim_start_matches('v').to_owned();
        let asset_name = format!("clice-{version}.{suffix}");

        let asset = release
            .assets
            .iter()
            .find(|asset| asset.name == asset_name)
            .ok_or_else(|| format!("release {version} has no asset named {asset_name}"))?;

        // Archives carry a top-level `clice/` directory holding `bin/`, `lib/clang`
        // and `clice.toml`; the server resolves its runtime files relative to the
        // executable, so the whole tree has to stay together.
        let version_dir = format!("{SERVER_NAME}-{version}");
        let binary_path = format!("{version_dir}/clice/bin/{executable}");

        if std::path::Path::new(&binary_path).exists() {
            zed::set_language_server_installation_status(
                language_server_id,
                &LanguageServerInstallationStatus::None,
            );
            return Ok(binary_path);
        }

        zed::set_language_server_installation_status(
            language_server_id,
            &LanguageServerInstallationStatus::Downloading,
        );

        let result = zed::download_file(&asset.download_url, &version_dir, file_type)
            .map_err(|error| format!("failed to download {asset_name}: {error}"))
            .and_then(|()| {
                zed::make_file_executable(&binary_path)
                    .map_err(|error| format!("failed to make {binary_path} executable: {error}"))
            });

        let status = match &result {
            Ok(()) => LanguageServerInstallationStatus::None,
            Err(error) => LanguageServerInstallationStatus::Failed(error.clone()),
        };
        zed::set_language_server_installation_status(language_server_id, &status);

        result?;

        Self::remove_outdated_versions(&version_dir);

        Ok(binary_path)
    }

    /// Drops previously downloaded versions once a newer one is installed.
    fn remove_outdated_versions(current: &str) {
        let Ok(entries) = std::fs::read_dir(".") else {
            return;
        };
        for entry in entries.flatten() {
            let name = entry.file_name();
            let Some(name) = name.to_str() else {
                continue;
            };
            if name.starts_with(SERVER_NAME) && name != current {
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
