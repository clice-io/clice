# Contexte d’en-tête

Pour que clangd fonctionne correctement, les utilisateurs doivent souvent fournir un fichier `compile_commands.json` (ci-après dénommé fichier CDB). L’unité de compilation de base du modèle de compilation traditionnel du C++ est un fichier source (par exemple, les fichiers `.c` et `.cpp`), où `#include` se contente de copier-coller le contenu des fichiers d’en-tête à la position correspondante dans le fichier source. Le fichier CDB mentionné précédemment stocke les commandes de compilation correspondant à chaque fichier source. Lorsque vous ouvrez un fichier source, clangd utilisera sa commande de compilation correspondante dans le CDB pour compiler ce fichier.

Naturellement, une question se pose : si le fichier CDB ne contient que des commandes de compilation pour les fichiers sources et non pour les fichiers d’en-tête, comment clangd traite-t-il les fichiers d’en-tête ? clangd traite les fichiers d’en-tête comme des fichiers sources, puis, selon certaines règles, utilise par exemple la commande de compilation d’un fichier source situé dans le même répertoire comme commande de compilation pour cet en-tête. Ce modèle est simple et efficace, mais il ignore certaines situations.

Puisque les fichiers d’en-tête font partie des fichiers sources, il arrive que leur contenu diffère selon ce qui les précède dans le fichier source. Par exemple :

```cpp
// a.h
#ifdef TEST
struct X { ... };
#else
struct Y { ... };
#endif

// b.cpp
#define TEST
#include "a.h"

// c.cpp
#include "a.h"
```

Évidemment, `a.h` a des états différents dans `b.cpp` et `c.cpp` — l’un définit `X` et l’autre définit `Y`. Si nous traitons simplement `a.h` comme un fichier source indépendant, alors seul `Y` pourra être vu.

Un cas plus extrême est celui des fichiers d’en-tête non autonomes (non « self-contained »), par exemple :

```cpp
// a.h
struct Y {
    X x;
};

// b.cpp
struct X {};
#include "a.h"
```

`a.h` ne peut pas être compilé seul, mais lorsqu’il est intégré dans `b.cpp`, il se compile normalement. Dans ce cas, clangd signalera une erreur dans `a.h`, incapable de trouver la définition de `X`. C’est parce qu’il traite `a.h` comme un fichier source indépendant. Il existe de nombreux fichiers d’en-tête de ce type dans le code de la libstdc++, et certaines bibliothèques C++ populaires composées uniquement d’en-tête (header-only) utilisent également ce genre de code, que clangd ne peut actuellement pas traiter.

clice prendra en charge le **contexte d’en-tête**, permettant le basculement automatique ou initié par l’utilisateur des états des fichiers d’en-tête, et supportera bien sûr les fichiers d’en-tête non autonomes. Nous voulons obtenir l’effet suivant, en utilisant le premier exemple de code : lorsque vous passez de `b.cpp` à `a.h`, `b.cpp` est utilisé comme contexte pour `a.h`. De même, lorsque vous passez de `c.cpp` à `a.h`, `c.cpp` est utilisé comme contexte pour `a.h`.
