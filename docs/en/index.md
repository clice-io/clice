---
layout: home

hero:
  name: clice
  text: Next Generation C++ Language Server
  tagline: Development is actively in progress
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
    src: /image.png
    alt: clice

features:
  - icon: 📝
    title: Compilation Context
    details: The first language server to introduce compilation context as a formal concept. Users can query and switch compilation contexts, with support for non-self-contained headers and multi-configuration projects
  - icon: 📦
    title: C++20 Modules
    details: Reference-counted real-time module compilation DAG with cancellation and dependency cascading. Code completion, semantic highlighting, and go-to-definition fully adapted for module syntax
  - icon: 🔍
    title: Template Resolution
    details: Resolves dependent names through pseudo-instantiation, providing accurate code completion and navigation even inside template definitions
  - icon: ⚡
    title: Multi-Process Architecture
    details: Master + Worker process model isolating Clang crashes and memory leaks. Supports priority scheduling, real-time memory monitoring, and automatic process recovery
---
