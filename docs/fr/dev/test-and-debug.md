# Test et débogage

## Exécuter les tests

clice possède deux types de tests : les tests unitaires et les tests d’intégration.

- Exécuter les tests unitaires

```bash
./build/bin/unit_tests --test-dir="./tests/data"
```

- Exécuter les tests d’intégration

Nous utilisons pytest pour exécuter les tests d’intégration. Veuillez vous référer à `pyproject.toml` pour installer les bibliothèques Python requises.

```bash
pytest -s --log-cli-level=INFO tests/integration --executable=./build/bin/clice
```

Si vous utilisez xmake comme système de construction, vous pouvez lancer les tests directement avec xmake :

```shell
xmake run --verbose unit_tests
xmake test --verbose integration_tests/default
```

## Débogage

Si vous souhaitez attacher un débogueur à clice, il est recommandé de démarrer d’abord clice en mode socket de manière indépendante, puis d’y connecter le client.

```shell
./build/bin/clice --mode=socket --port=50051
```

Une fois le serveur démarré, vous pouvez connecter un client au serveur de deux manières :

- Se connecter en lançant un test spécifique avec pytest

Vous pouvez lancer un seul cas de test d’intégration pour vous connecter à une instance clice en cours d’exécution. C’est très utile pour reproduire et déboguer des scénarios spécifiques.

```shell
pytest -s --log-cli-level=INFO tests/integration/test_file_operation.py::test_did_open --mode=socket --port=50051
```

- Utiliser VS Code pour des tests pratiques

Vous pouvez également vous connecter à un service clice en cours d’exécution en configurant l’extension clice-vscode, ce qui vous permet de déboguer dans un scénario d’utilisation réel.

1. Téléchargez l’extension [clice-vscode](https://marketplace.visualstudio.com/items?itemName=ykiko.clice-vscode) depuis le Marketplace.

2. Configurez `settings.json` : Créez un fichier `.vscode/settings.json` dans le répertoire racine de votre projet et ajoutez le contenu suivant :

    ```jsonc
    {
      // Pointez vers le binaire clice que vous avez téléchargé.
      "clice.executable": "/chemin/vers/votre/exécutable/clice",

      // Activez le mode socket.
      "clice.mode": "socket",
      "clice.port": 50051,

      // Optionnel : Définissez ceci sur une chaîne vide pour désactiver clangd.
      "clangd.path": "",
    }
    ```

3. Recharger la fenêtre : Après avoir modifié la configuration, exécutez la commande `Developer: Reload Window` dans VS Code pour que les paramètres prennent effet. L’extension se connectera automatiquement à l’instance clice écoutant sur le port 50051.

Si vous avez besoin de modifier ou de déboguer l’extension clice-vscode elle-même, suivez ces étapes :

1. Cloner et installer les dépendances :

    ```shell
    git clone https://github.com/clice-io/clice-vscode
    cd clice-vscode
    npm install
    ```

2. Ouvrir le projet d’extension avec VS Code : Ouvrez le dossier `clice-vscode` dans une nouvelle fenêtre VS Code.

3. Créer une configuration de débogage : Dans le projet `clice-vscode`, créez également un fichier `.vscode/settings.json` avec le même contenu que ci-dessus.

4. Appuyez sur `F5`. Cela lancera une fenêtre [Extension Development Host]. Il s’agit d’une nouvelle fenêtre VS Code avec le code de votre extension clice-vscode locale chargé. Ouvrez votre projet C++ dans cette nouvelle fenêtre, et elle devrait se connecter automatiquement à clice.
