# fast-kalman (vendored)

`kalman.hpp` is the header-only core of fast-kalman 0.3.0 (upstream commit
2cb7f38, 2026-08-15), by Adam DePrince, published under the Apache License 2.0
(see `LICENSE` in this directory). It is included unmodified. Goblin Skiff uses
its `LinearKalmanFilter` for the sender-side smoothing of hand motion reports;
see `src/terminal/handmotion.h`. Upstream: <https://fast-kalman.dev/>.
