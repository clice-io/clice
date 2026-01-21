# Démarrage rapide

## Extensions d’éditeur

clice implémente le [Language Server Protocol](https://microsoft.github.io/language-server-protocol). Tout éditeur prenant en charge ce protocole peut théoriquement fonctionner avec clice pour fournir des fonctionnalités telles que la `complétion de code`, les `diagnostics`, le `passage à la définition`, et plus encore.

Cependant, au-delà du protocole standard, clice prend également en charge certaines extensions de protocole. Pour une meilleure gestion de ces extensions et une meilleure intégration avec les éditeurs, l’utilisation de plugins clice dans des éditeurs spécifiques est souvent un meilleur choix. La plupart d’entre eux fonctionnent immédiatement et prennent en charge les extensions de protocole de clice.

### Visual Studio Code

### Vim/Neovim

### Autres

D’autres éditeurs n’ont pas encore d’extensions clice disponibles (les contributions sont les bienvenues !). Pour utiliser clice avec eux, veuillez installer clice vous-même et vous référer à la documentation de votre éditeur spécifique sur la façon d’utiliser un serveur de langage.

## Installation

Si votre plugin d’éditeur gère le téléchargement de clice, vous pouvez ignorer cette étape.

### Télécharger le binaire précompilé

Téléchargez la version binaire de clice via la page des Releases.

### Construire à partir des sources

Construisez clice à partir des sources vous-même. Pour les étapes spécifiques, reportez-vous à [construction](../dev/build.md).

## Configuration du projet

Pour que clice comprenne correctement votre code (par exemple, pour trouver l’emplacement des fichiers d’en-tête), vous devez fournir à clice un fichier `compile_commands.json`, également connu sous le nom de [base de données de compilation](https://clang.llvm.org/docs/JSONCompilationDatabase.html). La base de données de compilation fournit les options de compilation pour chaque fichier source.

### CMake

Pour les systèmes de construction utilisant CMake, ajoutez l’option `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` lors de la construction, par exemple :

```cmake
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Cela générera un fichier `compile_commands.json` dans le répertoire `build`.

::: warning
Note : Cette option ne fonctionne que lorsque le générateur de CMake est défini sur Makefile ou Ninja. Pour les autres générateurs, cette option sera ignorée, ce qui signifie que la base de données de compilation ne pourra pas être générée.
:::

### Bazel

Bazel n’a pas de support natif pour générer une base de données de compilation. La solution recommandée est d’utiliser [bazel-compile-commands-extractor](https://github.com/hedronvision/bazel-compile-commands-extractor). Après l’avoir configuré, vous pouvez générer `compile_commands.json` avec :

```bash
bazel run @hedron_compile_commands//:refresh_all
```

### Visual Studio

TODO :

### Makefile

TODO :

### Xmake

### Autres

Pour tout autre système de construction, vous pouvez essayer d’utiliser [bear](https://github.com/rizsotto/Bear) ou [scan-build](https://github.com/rizsotto/scan-build) pour intercepter les commandes de compilation et obtenir la base de données de compilation (sans garantie de succès). Nous prévoyons d’écrire un **nouvel outil** à l’avenir qui capturera les commandes de compilation via une approche de faux compilateur.
