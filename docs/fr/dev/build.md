# Construire à partir des sources

clice dépend des fonctionnalités de C++23 et nécessite une chaîne d'outils C++ moderne. Nous devons également effectuer l'édition de liens avec LLVM/Clang pour analyser les AST. Pour accélérer les constructions, la configuration par défaut télécharge notre paquet précompilé [clice-llvm](https://github.com/clice-io/clice-llvm). Cela suppose que votre environnement local corresponde étroitement à l'environnement de précompilation (en particulier lors de l'activation de l'Address Sanitizer ou de l'LTO).

Pour simplifier la configuration et garantir la reproductibilité des constructions, nous **recommandons fortement** [pixi](https://pixi.prefix.dev/latest) pour gérer l'environnement de développement. Les versions des dépendances sont figées dans `pixi.toml`.

Si vous préférez ne pas utiliser pixi, consultez la section [Construction Manuelle](#construction-manuelle) ci-dessous.

## Démarrage Rapide

Installez pixi en suivant le [guide officiel](https://pixi.prefix.dev/latest/installation).

Nous proposons plusieurs tâches ; les commandes ci-dessous configurent, construisent et lancent les tests :

```shell
# configurer && construire (par défaut RelWithDebInfo)
pixi run build

# tests unitaires && d'intégration
pixi run test
```

Pour des tâches plus précises (le premier argument définit le type de construction) :

```shell
pixi run cmake-config Debug
pixi run cmake-build Debug
pixi run unit-test Debug
pixi run integration-test Debug
```

> [!TIP]
> Si vous souhaitez développer directement avec `cmake`, `ninja`, `clang++`, etc., lancez `pixi shell` pour entrer dans un shell où toutes les variables d'environnement sont configurées.

### XMake

Nous prenons également en charge la construction avec XMake :

```shell
# config & build (par défaut releasedbg)
pixi run xmake

# tests unitaires & d'intégration
pixi run xmake-test
```

## Construction Manuelle

Si vous prévoyez de construire manuellement, assurez-vous d'abord que votre chaîne d'outils correspond aux versions définies dans `pixi.toml`.

> Compatibilité : En théorie, clice ne dépend pas d'extensions spécifiques au compilateur, donc les compilateurs grand public (GCC/Clang/MSVC) devraient fonctionner. Cependant, l'intégration continue (CI) ne garantit que des versions spécifiques de Clang. Les autres compilateurs ou versions sont pris en charge au **mieux possible**. Veuillez ouvrir une issue ou une PR si vous rencontrez des problèmes.

### CMake

```shell
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake \
    -DCLICE_ENABLE_TEST=ON
```

> Note : `CMAKE_TOOLCHAIN_FILE` est optionnel. Si votre chaîne d'outils correspond exactement à la nôtre, vous pouvez utiliser le fichier `cmake/toolchain.cmake` prédéfini ; sinon, retirez ce drapeau.

Options de construction optionnelles :

| Option               | Défaut | Effet                                                                                                                         |
| -------------------- | ------ | ----------------------------------------------------------------------------------------------------------------------------- |
| LLVM_INSTALL_PATH    | ""     | Construire clice avec LLVM depuis un chemin personnalisé                                                                      |
| CLICE_ENABLE_TEST    | OFF    | Construire les tests unitaires de clice                                                                                       |
| CLICE_USE_LIBCXX     | OFF    | Construire clice avec libc++ (ajoute `-std=libc++`) ; si activé, assurez-vous que les libs LLVM sont aussi construites avec libc++ |
| CLICE_CI_ENVIRONMENT | OFF    | Activer la macro `CLICE_CI_ENVIRONMENT` ; certains tests ne s'exécutent qu'en CI                                              |

### XMake

Construisez clice avec :

```bash
xmake f -c --mode=releasedbg --toolchain=clang
xmake build --all
```

Options de construction optionnelles :

| Option        | Défaut | Effet                                                   |
| ------------- | ------ | ------------------------------------------------------- |
| --llvm        | ""     | Construire clice avec LLVM depuis un chemin personnalisé |
| --enable_test | false  | Construire les tests unitaires de clice                 |
| --ci          | false  | Activer `CLICE_CI_ENVIRONMENT`                          |

## À propos de LLVM

clice appelle les API Clang pour analyser le code C++, il doit donc être lié à LLVM/Clang. Comme clice utilise des en-têtes privés de Clang (généralement absents des paquets de distribution), le paquet LLVM du système ne peut pas être utilisé directement.

Deux façons de satisfaire cette dépendance :

1. Nous publions des binaires précompilés de la version de LLVM que nous utilisons sur [clice-llvm](https://github.com/clice-io/clice-llvm/releases) pour la CI et les versions de sortie. Lors des constructions, cmake et xmake téléchargent ces bibliothèques LLVM par défaut.

> [!IMPORTANT]
>
> Pour les constructions LLVM en mode debug, nous activons l'Address Sanitizer, qui dépend de compiler-rt et est très sensible à la version du compilateur. Si vous utilisez une construction debug, assurez-vous que votre version de clang compiler-rt correspond à celle définie dans `pixi.toml`.

2. Construisez LLVM/Clang vous-même pour correspondre à votre environnement. Si les binaires précompilés par défaut échouent en raison d'une inadéquation de l'ABI ou des versions de bibliothèques, ou si vous avez besoin d'une construction debug personnalisée, utilisez cette approche. Nous fournissons `scripts/build-llvm.py` pour construire les bibliothèques LLVM requises, ou reportez-vous au guide officiel de LLVM [Building LLVM with CMake](https://llvm.org/docs/CMake.html).
