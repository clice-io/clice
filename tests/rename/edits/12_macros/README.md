# Rename a macro

Renaming a macro edits its `#define` in each branch of the `#if` it is defined in, the `#ifndef` guarding a default, and every use, written as another macro's argument or in another macro's body; a use pasted together with `##` is left alone and named in a message
