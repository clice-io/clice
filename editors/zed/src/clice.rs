use zed_extension_api::{self as zed, LanguageServerId, Result, Worktree};

struct CliceExtension;

struct CliceBinary {
    path: String,
}

impl CliceExtension {
    fn find_clice_binary(&self, worktree: &Worktree) -> Result<CliceBinary> {
        if let Some(path_str) = worktree.which("clice") {
            Ok(CliceBinary { path: path_str })
        } else {
            Err(
                "`clice` not found in your PATH. Please install it and add it to your system's PATH environment variable.".to_string()
            )
        }
    }
}

impl zed::Extension for CliceExtension {
    fn new() -> Self {
        Self
    }

    // Currently, we only search for the 'clice' binary in the system's PATH.
    fn language_server_command(
        &mut self,
        _language_server_id: &LanguageServerId,
        worktree: &Worktree,
    ) -> Result<zed::Command> {
        let binary = self.find_clice_binary(worktree)?;
        Ok(zed::Command {
            command: binary.path,
            args: vec![
                "--mode".to_string(),
                "pipe".to_string(),
            ],
            env: Default::default(),
        })
    }
}

zed::register_extension!(CliceExtension);
