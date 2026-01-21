# Configuration

Ceci est la documentation pour `clice.toml`.

## Projet

| Nom                 | Type     | Par défaut                    |
| ------------------- | -------- | ----------------------------- |
| `project.cache_dir` | `string` | `"${workspace}/.clice/cache"` |

Dossier pour stocker les caches PCH et PCM.
<br>

| Nom                 | Type     | Par défaut                    |
| ------------------- | -------- | ----------------------------- |
| `project.index_dir` | `string` | `"${workspace}/.clice/index"` |

Dossier pour stocker les fichiers d'index.
<br>

## Règle

`[[rules]]` représente un tableau d'objets, où chaque objet possède les propriétés suivantes :
<br>

| Nom                | Type                 |
| ------------------ | -------------------- |
| `[rules].patterns` | `array` de `strings` |

Modèles glob pour faire correspondre les chemins de fichiers, suivant le [standard](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#documentFilter) du LSP.

- `*` : Correspond à un ou plusieurs caractères dans un segment de chemin.
- `?` : Correspond à un seul caractère dans un segment de chemin.
- `**` : Correspond à n'importe quel nombre de segments de chemin, y compris zéro.
- `{}` : Utilisé pour grouper des conditions (par exemple, `**/*.{ts,js}` correspond à tous les fichiers TypeScript et JavaScript).
- `[]` : Déclare une plage de caractères à faire correspondre dans un segment de chemin (par exemple, `exemple.[0-9]` correspond à `exemple.0`, `exemple.1`, etc.).
- `[!...]` : Exclut une plage de caractères à faire correspondre dans un segment de chemin (par exemple, `exemple.[!0-9]` correspond à `exemple.a`, `exemple.b`, mais pas à `exemple.0`).
  <br>

| Nom              | Type                 | Par défaut |
| ---------------- | -------------------- | ---------- |
| `[rules].append` | `array` de `strings` | `[]`       |

Commandes à ajouter à la liste de commandes originale. Par exemple, `append = ["-std=c++17"]`.
<br>

| Nom              | Type                 | Par défaut |
| ---------------- | -------------------- | ---------- |
| `[rules].remove` | `array` de `strings` | `[]`       |

Commandes à supprimer de la liste de commandes originale. Par exemple, `remove = ["-std=c++11"]`.
<br>
