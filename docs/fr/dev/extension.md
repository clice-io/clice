# Extension

Cette section couvre les flux de travail de développement et de publication pour les extensions d’éditeur (VSCode / Neovim / Zed).

## VSCode

L’extension VSCode utilise la chaîne d’outils Node/PNPM/VSCE. Travaillez dans l’environnement pixi `node` pour des versions cohérentes.

```shell
# préparer l’environnement (installez pixi d’abord)
pixi shell -e node

# installer les dépendances (utilise pnpm-lock)
pixi run install-vscode

# packager l’extension ; génère editors/vscode/*.vsix
pixi run build-vscode
```

Publier sur le VSCode Marketplace (variable d’env `VSCE_PAT` requise) :

```shell
pixi run publish-vscode
```

> [!TIP]
> Si clice est déjà construit localement, définissez `clice.executable` dans les paramètres VSCode pour pointer l’extension vers votre binaire personnalisé.

Développement et débogage :

1. `pixi shell -e node`
2. Dans `editors/vscode`, lancez `pnpm run watch` pour des constructions incrémentielles
3. Dans VSCode, utilisez les configurations “Run Extension/Launch Extension”, ou lancez `code --extensionDevelopmentPath=$(pwd)/editors/vscode`

Scripts courants (à l’intérieur de `pixi shell -e node`) :

```bash
pnpm run package # identique à pixi run build-vscode
pnpm run publish # identique à pixi run publish-vscode
```

Si vous n’utilisez pas pixi, installez Node.js >= 20 et pnpm vous-même, puis dans `editors/vscode`, lancez :

```bash
pnpm install
pnpm run package
```

## Neovim

L’extension Neovim se trouve dans `editors/nvim` et est écrite en Lua. Elle est toujours en cours d’évolution.

- Ajoutez le chemin du dépôt au `runtimepath`, par exemple : `set rtp+=/chemin/vers/clice/editors/nvim`
- Ou créez un lien symbolique local : `~/.config/nvim/pack/clice/start/clice` -> `<repo>/editors/nvim`
- Assurez-vous que l’exécutable `clice` est accessible dans votre `$PATH`

Conseils de dév : le code est léger — chargez-le directement dans Neovim et surveillez les logs `:messages`/LSP ; formatez avec `stylua` (configuration incluse).

## Zed

L’extension Zed se trouve dans `editors/zed` et utilise Rust ainsi que `zed_extension_api`.

Vérification locale suggérée :

```bash
cd editors/zed
cargo build --release
```

Ensuite, chargez l’extension locale selon le guide officiel de Zed (CLI Zed requis). Assurez-vous que `clice` est dans le `PATH` avant le lancement. Suivez le flux de publication des extensions Zed lors de la sortie d’une version.
