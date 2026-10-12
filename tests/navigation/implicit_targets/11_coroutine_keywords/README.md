# `co_await` / `co_yield` / `co_return`

Coroutine keywords navigate to the methods they call

Go-to-definition on `co_await` reaches the awaiter's `await_ready`,
`await_suspend` and `await_resume`; on `co_yield`, the promise's
`yield_value`; on `co_return`, the promise's `return_value`.
