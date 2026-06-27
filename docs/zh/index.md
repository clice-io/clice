---
layout: home

hero:
  name: clice
  text: 下一代 C++ 语言服务器
  tagline: 开发正在活跃进行中
  actions:
    - theme: brand
      text: 什么是 clice?
      link: ./guide/what-is-clice
    - theme: alt
      text: 快速开始
      link: ./guide/quick-start
    - theme: alt
      text: 参与贡献
      link: ./dev/contribution
  image:
    src: /image.png
    alt: clice

features:
  - icon: 📝
    title: 编译上下文
    details: 第一个将编译上下文作为正式概念的语言服务器。用户可以查询和切换编译上下文，支持非自包含头文件和多配置项目
  - icon: 📦
    title: C++20 模块
    details: 基于引用计数的实时模块编译 DAG，支持取消和依赖级联。代码补全、语义高亮、跳转定义全面适配模块语法
  - icon: 🔍
    title: 模板解析
    details: 通过伪实例化技术解析依赖名称，在模板定义内部也能提供准确的代码补全和跳转
  - icon: ⚡
    title: 多进程架构
    details: Master + Worker 进程模型，隔离 Clang 的崩溃和内存泄漏。支持优先级调度、实时内存监控和进程自动恢复
---
