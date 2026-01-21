# Compilation

## Analyse Incrémentale

Chaque fois que vous modifiez du code, clice doit ré-analyser le fichier. clice utilise un mécanisme appelé "préambule" pour implémenter la compilation incrémentale afin d'accélérer la ré-analyse. Le préambule peut être considéré comme un cas particulier d'un [En-tête Précompilé](https://clang.llvm.org/docs/PCHInternals.html) (PCH) intégré aux fichiers sources. Lors de l'ouverture d'un fichier, clice compile les premières directives du préprocesseur au début du fichier (appelées préambule) dans un cache PCH sur le disque. Plus tard, lors de l'analyse, il peut charger directement le fichier PCH, sautant ainsi les premières directives, ce qui réduit considérablement la quantité de code à ré-analyser.

Par exemple, pour le code suivant :

```cpp
#include <iostream>

int main () {
    std::cout << "Hello world!" << std::endl;
}
```

Le fichier d'en-tête `iostream` contient environ 20 000 lignes de code. clice va d'abord compiler la ligne `#include <iostream>` dans un fichier PCH, et une fois terminé, utiliser ce fichier PCH pour analyser le code suivant. De cette façon, la quantité de code à ré-analyser ultérieurement est réduite à seulement 5 lignes au lieu des 20 000 lignes originales, ce qui rend l'opération très rapide. Sauf si vous modifiez le code dans la section du préambule, ce qui nécessiterait la construction d'un nouveau préambule.
