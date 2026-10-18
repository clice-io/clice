import { answers } from "@clice/tools/testing";

// Completing inside a call that a unary operator applies to, where an
// overloaded operator- is in scope: clang's thread-safety analysis of the
// function being completed crashes on what the completion point leaves of
// its body, unless the completion compile ignores warnings.
answers(async (a) => {
    const inside = a.marker("inside");
    await a.show("completion", a.completion(inside));
    await a.show("signature help", a.signatureHelp(inside));
});
