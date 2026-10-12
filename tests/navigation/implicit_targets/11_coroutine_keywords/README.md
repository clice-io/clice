# `co_await` / `co_yield` / `co_return`

`co_yield` navigates to its promise method, while other coroutine keywords
do not

Go-to-definition on `co_yield` reaches the promise's `yield_value`. The
`co_await` and `co_return` keywords do not yet reach the awaiter's or
promise's methods.
