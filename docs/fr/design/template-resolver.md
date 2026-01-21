# Résolveur de Template

Tout d’abord, il y a un meilleur support des templates, ce qui est aussi la fonctionnalité que je voulais initialement que clangd prenne en charge. Plus précisément, quels sont les problèmes actuels dans la gestion des templates ?

Prenons la complétion de code comme exemple. Considérez le code suivant, où `^` représente la position du curseur :

```cpp
template <typename T>
void foo(std::vector<T> vec) {
    vec.^
}
```

En C++, si un type dépend de paramètres de template, nous ne pouvons faire aucune supposition précise à son sujet avant l’instanciation du template. Par exemple, ici `vector` pourrait être soit le template primaire, soit la spécialisation partielle de `vector<bool>`. Lequel devrions-nous choisir ? Pour la compilation du code, la précision est toujours le plus important — nous ne pouvons utiliser aucun résultat susceptible de conduire à des erreurs. Mais, pour les serveurs de langage, fournir plus de résultats possibles est souvent mieux que de ne rien fournir du tout. Nous pouvons supposer que les utilisateurs utilisent le template primaire plus souvent que les spécialisations partielles, et donc fournir des résultats de complétion de code basés sur le template primaire. Actuellement, clangd fait exactement cela — dans le cas ci-dessus, il fournira une complétion de code basée sur le template primaire de `vector`.

Considérez un exemple plus complexe :

```cpp
template <typename T>
void foo(std::vector<std::vector<T>> vec2) {
    vec2[0].^
}
```

Du point de vue de l’utilisateur, la complétion devrait également être fournie ici, puisque le type de `vec2[0]` est aussi `vector<T>`, n’est-ce pas ? Même chose que l’exemple précédent. Mais, clangd ne fournira aucune complétion ici. Quel est le problème ? Selon la norme C++, le type de retour de `std::vector<T>::operator[]` est `std::vector<T>::reference`, qui est en fait un [nom dépendant](https://en.cppreference.com/w/cpp/language/dependent_name). Son résultat semble assez direct — c’est T&. Mais, dans libstdc++, sa définition est imbriquée dans des dizaines de couches de templates, apparemment pour la compatibilité avec les anciennes normes ? Alors, pourquoi clangd ne peut-il pas traiter cette situation ?

1. Il s’appuie sur des hypothèses de template primaire, sans considérer que les spécialisations partielles pourraient rendre la recherche impossible.
2. Seule une recherche de nom est effectuée, sans instanciation de template. Par conséquent, même s’il trouve le résultat final, il ne peut pas le relier aux paramètres de template originaux.
3. Les paramètres de template par défaut ne sont pas pris en compte, ce qui rend impossible la gestion des noms dépendants causés par ceux-ci.

Bien que nous puissions faire des exceptions pour les types de la bibliothèque standard afin de fournir un support connexe, je souhaite que le code utilisateur puisse avoir le même statut que le code de la bibliothèque standard, nous avons donc besoin d’un algorithme universel pour traiter les types dépendants. Pour résoudre ce problème, j’ai écrit une pseudo-instanciation (pseudo instantiator). Elle peut instancier des types dépendants sans types spécifiques, atteignant ainsi un objectif de simplification. Par exemple, dans l’exemple ci-dessus, `std::vector<std::vector<T>>::reference` peut être simplifié en `std::vector<T>&`, ce qui permet de fournir des options de complétion de code pour les utilisateurs.
