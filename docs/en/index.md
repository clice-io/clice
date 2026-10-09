---
layout: home

hero:
  name: clice
  text: Next Generation C++ Language Server
  tagline: Fast, crash-proof and project-aware. Pre-release builds ship nightly for VS Code, Neovim and Zed.
  actions:
    - theme: brand
      text: What is clice?
      link: ./guide/what-is-clice
    - theme: alt
      text: Quick Start
      link: ./guide/quick-start
    - theme: alt
      text: Contribution
      link: ./dev/contribution
  image:
    src: /mascot/hero-working.webp
    alt: clice

features:
  - icon:
      src: /icons/clice-context.webp
    title: Compilation Context
    details: The first language server to introduce compilation context as a formal concept. Users can query and switch compilation contexts, with support for non-self-contained headers and multi-configuration projects
  - icon:
      src: /icons/clice-modules.webp
    title: C++20 Modules
    details: Reference-counted real-time module compilation DAG with cancellation and dependency cascading. Code completion, semantic highlighting, and go-to-definition fully adapted for module syntax
  - icon:
      src: /icons/clice-templates.webp
    title: Template Resolution
    details: Resolves dependent names through pseudo-instantiation, providing accurate code completion and navigation even inside template definitions
  - icon:
      src: /icons/clice-processes.webp
    title: Multi-Process Architecture
    details: Master + Worker process model isolating Clang crashes and memory leaks. Supports priority scheduling, real-time memory monitoring, and automatic process recovery
---
