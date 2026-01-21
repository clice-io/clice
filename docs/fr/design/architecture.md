# Architecture

## Protocol

Utilise le C++ pour décrire les définitions de types dans le [Language Server Protocol](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/).

## AST

Quelques wrappers pratiques pour les interfaces AST de Clang.

## Async

Wrapper pour les coroutines libuv utilisant les coroutines C++20.

## Compilateur

Wrapper pour les interfaces de compilation Clang, responsable des processus de compilation réels et de l'obtention de diverses informations de compilation.

## Feature

Implémentations spécifiques des diverses fonctionnalités LSP.

## Server

clice est un serveur de langage, et avant tout un serveur. Il utilise [libuv](https://github.com/libuv/libuv) comme bibliothèque d'événements, adoptant un modèle de compilation piloté par événements commun. Le thread principal est responsable de la gestion des requêtes et de la distribution des tâches, tandis que le pool de threads est responsable de l'exécution des tâches chronophages, telles que les tâches de compilation. Le code associé se trouve dans le répertoire `Server`.

## Support

Quelques autres bibliothèques utilitaires.
