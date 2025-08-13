---
layout: home

hero:
  name: clice
  text: Next Generation C++ Language Server
  tagline: Development is actively in progress
  actions:
    - theme: brand
      text: What is clice?
      link: /en/guide/what-is-clice
    - theme: alt
      text: Quick Start
      link: /en/guide/quick-start
    - theme: alt
      text: Contribution
      link: /en/dev/contribution
  image:
      src: /image.png
      alt: clice

features:
  - icon: T
    title: Better Template Handling
    details: Use pseudo-instantiation to handle dependent template names, with code completion even for complex templates
  - icon: H
    title: Header File Context
    details: Support header file state switching between different source file contexts, and fully support non-self-contained files
  - icon: M
    title: Modules
    details: Excellent C++20 module support, from code completion to highlighting to navigation, all adapted
  - icon: I
    title: Better Performance
    details: Excellent asynchronous task scheduling, support for compilation task cancellation, caching necessary information, avoiding meaningless CPU waste
---