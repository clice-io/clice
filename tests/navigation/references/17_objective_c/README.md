# Objective-C messages

The references of a method include every message that may reach it: sent to its class or a subclass, through a protocol it implements, or to `id`

A message to `id` or `Class` may reach any method of its selector, so it is
among the references of each; a message to `super` reaches the superclass's
method alone. A message spelling a selector in several pieces is listed once,
at its first.
