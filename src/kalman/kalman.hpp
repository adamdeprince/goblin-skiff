#pragma once

// kalman.hpp
// C++23 fixed-size Kalman filters for tiny state/measurement dimensions.
//
// Design goals:
//   * double precision only
//   * compile-time dimensions; no dynamic allocation in the core filters
//   * row-major contiguous storage suitable for compiler auto-vectorization
//   * explicit P/Q/R covariance semantics; no hidden noise-speed parameter coupling
//   * generic fixed-size implementation plus small scalar-measurement hot path
//   * no external matrix library
//
// Most methods are defined in the header because they are templates.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kalman {

using Scalar = double;

inline constexpr Scalar kDefaultEps = 1.0e-15;
inline constexpr Scalar kDefaultJitter = 1.0e-12;
inline constexpr Scalar kDefaultVarianceFloor = 1.0e-15;

[[nodiscard]] const char* version() noexcept;

struct LinearTuningRecommendation {
    std::string model;
    std::vector<Scalar> x;
    std::vector<Scalar> P;
    std::vector<Scalar> F;
    std::vector<Scalar> Q;
    std::vector<Scalar> H;
    std::vector<Scalar> R;
    std::vector<Scalar> P_diag;
    std::vector<Scalar> Q_diag;
    std::vector<Scalar> R_diag;
};

[[nodiscard]] LinearTuningRecommendation linear_recommendation(
    std::span<const Scalar> samples,
    std::size_t sample_count,
    std::size_t nx,
    std::size_t nz,
    Scalar dt = 1.0,
    std::string_view model = "auto",
    Scalar minimum_variance = 1.0e-12,
    std::span<const Scalar> delta_t = {});

template <std::size_t N>
struct Vec {
    static_assert(N > 0);
    std::array<Scalar, N> a{};

    constexpr Vec() noexcept = default;
    constexpr explicit Vec(Scalar fill) noexcept { a.fill(fill); }
    constexpr Vec(std::initializer_list<Scalar> init) noexcept {
        std::size_t i = 0;
        for (Scalar x : init) {
            if (i < N) a[i++] = x;
        }
        for (; i < N; ++i) a[i] = 0.0;
    }

    [[nodiscard]] constexpr Scalar* data() noexcept { return a.data(); }
    [[nodiscard]] constexpr const Scalar* data() const noexcept { return a.data(); }
    [[nodiscard]] static constexpr std::size_t size() noexcept { return N; }

    [[nodiscard]] constexpr Scalar& operator[](std::size_t i) noexcept { return a[i]; }
    [[nodiscard]] constexpr Scalar operator[](std::size_t i) const noexcept { return a[i]; }
    constexpr void fill(Scalar x) noexcept { a.fill(x); }
};

template <std::size_t R, std::size_t C>
struct Mat {
    static_assert(R > 0 && C > 0);
    std::array<Scalar, R * C> a{};

    constexpr Mat() noexcept = default;
    constexpr explicit Mat(Scalar fill) noexcept { a.fill(fill); }
    constexpr Mat(std::initializer_list<Scalar> init) noexcept {
        std::size_t i = 0;
        for (Scalar x : init) {
            if (i < R * C) a[i++] = x;
        }
        for (; i < R * C; ++i) a[i] = 0.0;
    }

    [[nodiscard]] constexpr Scalar* data() noexcept { return a.data(); }
    [[nodiscard]] constexpr const Scalar* data() const noexcept { return a.data(); }
    [[nodiscard]] static constexpr std::size_t rows() noexcept { return R; }
    [[nodiscard]] static constexpr std::size_t cols() noexcept { return C; }

    [[nodiscard]] constexpr Scalar& operator()(std::size_t r, std::size_t c) noexcept { return a[r * C + c]; }
    [[nodiscard]] constexpr Scalar operator()(std::size_t r, std::size_t c) const noexcept { return a[r * C + c]; }
    constexpr void fill(Scalar x) noexcept { a.fill(x); }

    [[nodiscard]] static constexpr Mat identity() noexcept requires (R == C) {
        Mat out{};
        for (std::size_t i = 0; i < R; ++i) out(i, i) = 1.0;
        return out;
    }

    [[nodiscard]] static constexpr Mat diagonal(Scalar x) noexcept requires (R == C) {
        Mat out{};
        for (std::size_t i = 0; i < R; ++i) out(i, i) = x;
        return out;
    }
};

template <std::size_t N>
[[nodiscard]] constexpr Vec<N> operator+(const Vec<N>& x, const Vec<N>& y) noexcept {
    Vec<N> out{};
    for (std::size_t i = 0; i < N; ++i) out[i] = x[i] + y[i];
    return out;
}

template <std::size_t N>
[[nodiscard]] constexpr Vec<N> operator-(const Vec<N>& x, const Vec<N>& y) noexcept {
    Vec<N> out{};
    for (std::size_t i = 0; i < N; ++i) out[i] = x[i] - y[i];
    return out;
}

template <std::size_t N>
constexpr Vec<N>& operator+=(Vec<N>& x, const Vec<N>& y) noexcept {
    for (std::size_t i = 0; i < N; ++i) x[i] += y[i];
    return x;
}

template <std::size_t N>
constexpr Vec<N>& operator-=(Vec<N>& x, const Vec<N>& y) noexcept {
    for (std::size_t i = 0; i < N; ++i) x[i] -= y[i];
    return x;
}

template <std::size_t N>
[[nodiscard]] constexpr Vec<N> operator*(Scalar s, const Vec<N>& x) noexcept {
    Vec<N> out{};
    for (std::size_t i = 0; i < N; ++i) out[i] = s * x[i];
    return out;
}

template <std::size_t N>
[[nodiscard]] constexpr Vec<N> operator*(const Vec<N>& x, Scalar s) noexcept { return s * x; }

template <std::size_t N>
constexpr Vec<N>& operator*=(Vec<N>& x, Scalar s) noexcept {
    for (std::size_t i = 0; i < N; ++i) x[i] *= s;
    return x;
}

template <std::size_t R, std::size_t C>
[[nodiscard]] constexpr Mat<R, C> operator+(const Mat<R, C>& x, const Mat<R, C>& y) noexcept {
    Mat<R, C> out{};
    for (std::size_t i = 0; i < R * C; ++i) out.a[i] = x.a[i] + y.a[i];
    return out;
}

template <std::size_t R, std::size_t C>
[[nodiscard]] constexpr Mat<R, C> operator-(const Mat<R, C>& x, const Mat<R, C>& y) noexcept {
    Mat<R, C> out{};
    for (std::size_t i = 0; i < R * C; ++i) out.a[i] = x.a[i] - y.a[i];
    return out;
}

template <std::size_t R, std::size_t C>
constexpr Mat<R, C>& operator+=(Mat<R, C>& x, const Mat<R, C>& y) noexcept {
    for (std::size_t i = 0; i < R * C; ++i) x.a[i] += y.a[i];
    return x;
}

template <std::size_t R, std::size_t C>
constexpr Mat<R, C>& operator-=(Mat<R, C>& x, const Mat<R, C>& y) noexcept {
    for (std::size_t i = 0; i < R * C; ++i) x.a[i] -= y.a[i];
    return x;
}

template <std::size_t R, std::size_t C>
[[nodiscard]] constexpr Mat<R, C> operator*(Scalar s, const Mat<R, C>& x) noexcept {
    Mat<R, C> out{};
    for (std::size_t i = 0; i < R * C; ++i) out.a[i] = s * x.a[i];
    return out;
}

template <std::size_t R, std::size_t C>
[[nodiscard]] constexpr Mat<R, C> operator*(const Mat<R, C>& x, Scalar s) noexcept { return s * x; }

template <std::size_t R, std::size_t C>
constexpr Mat<R, C>& operator*=(Mat<R, C>& x, Scalar s) noexcept {
    for (std::size_t i = 0; i < R * C; ++i) x.a[i] *= s;
    return x;
}

template <std::size_t N>
[[nodiscard]] constexpr Scalar dot(const Vec<N>& x, const Vec<N>& y) noexcept {
    Scalar out = 0.0;
    for (std::size_t i = 0; i < N; ++i) out += x[i] * y[i];
    return out;
}

template <std::size_t N>
[[nodiscard]] inline Scalar norm2(const Vec<N>& x) noexcept { return dot(x, x); }

template <std::size_t N>
[[nodiscard]] inline Scalar norm(const Vec<N>& x) noexcept { return std::sqrt(norm2(x)); }

template <std::size_t R, std::size_t C>
[[nodiscard]] constexpr Mat<C, R> transpose(const Mat<R, C>& x) noexcept {
    Mat<C, R> out{};
    for (std::size_t r = 0; r < R; ++r) {
        for (std::size_t c = 0; c < C; ++c) out(c, r) = x(r, c);
    }
    return out;
}

template <std::size_t N>
[[nodiscard]] constexpr Mat<1, N> transpose(const Vec<N>& x) noexcept {
    Mat<1, N> out{};
    for (std::size_t i = 0; i < N; ++i) out(0, i) = x[i];
    return out;
}

template <std::size_t R, std::size_t K, std::size_t C>
[[nodiscard]] constexpr Mat<R, C> operator*(const Mat<R, K>& x, const Mat<K, C>& y) noexcept {
    // Loop order r-k-c so the inner loop has unit-stride reads on y(k, .) and
    // unit-stride writes on out(r, .). Better for autovectorization than r-c-k.
    Mat<R, C> out{};
    for (std::size_t r = 0; r < R; ++r) {
        for (std::size_t k = 0; k < K; ++k) {
            const Scalar xrk = x(r, k);
            for (std::size_t c = 0; c < C; ++c) out(r, c) += xrk * y(k, c);
        }
    }
    return out;
}

template <std::size_t R, std::size_t C>
[[nodiscard]] constexpr Vec<R> operator*(const Mat<R, C>& x, const Vec<C>& y) noexcept {
    Vec<R> out{};
    for (std::size_t r = 0; r < R; ++r) {
        Scalar sum = 0.0;
        for (std::size_t c = 0; c < C; ++c) sum += x(r, c) * y[c];
        out[r] = sum;
    }
    return out;
}

template <std::size_t R, std::size_t C>
[[nodiscard]] constexpr Mat<R, C> outer(const Vec<R>& x, const Vec<C>& y) noexcept {
    Mat<R, C> out{};
    for (std::size_t r = 0; r < R; ++r) {
        for (std::size_t c = 0; c < C; ++c) out(r, c) = x[r] * y[c];
    }
    return out;
}

// Fused A · Bᵀ without materializing the transpose. A is R×K, B is C×K.
// Loop order r-c-k makes both inner reads (A(r,·), B(c,·)) unit-stride.
template <std::size_t R, std::size_t K, std::size_t C>
[[nodiscard]] constexpr Mat<R, C> mul_AB_T(const Mat<R, K>& a, const Mat<C, K>& b) noexcept {
    Mat<R, C> out{};
    for (std::size_t r = 0; r < R; ++r) {
        for (std::size_t c = 0; c < C; ++c) {
            Scalar sum = 0.0;
            for (std::size_t k = 0; k < K; ++k) sum += a(r, k) * b(c, k);
            out(r, c) = sum;
        }
    }
    return out;
}

// Fused Aᵀ · B without materializing the transpose. A is K×R, B is K×C.
// Loop order k-r-c keeps the innermost loop (over c) unit-stride on B and out.
template <std::size_t R, std::size_t K, std::size_t C>
[[nodiscard]] constexpr Mat<R, C> mul_AT_B(const Mat<K, R>& a, const Mat<K, C>& b) noexcept {
    Mat<R, C> out{};
    for (std::size_t k = 0; k < K; ++k) {
        for (std::size_t r = 0; r < R; ++r) {
            const Scalar akr = a(k, r);
            for (std::size_t c = 0; c < C; ++c) out(r, c) += akr * b(k, c);
        }
    }
    return out;
}

template <std::size_t N>
constexpr void symmetrize(Mat<N, N>& x) noexcept {
    for (std::size_t r = 0; r < N; ++r) {
        for (std::size_t c = r + 1; c < N; ++c) {
            const Scalar v = 0.5 * (x(r, c) + x(c, r));
            x(r, c) = v;
            x(c, r) = v;
        }
    }
}

template <std::size_t N>
constexpr void floor_diagonal(Mat<N, N>& x, Scalar floor = kDefaultVarianceFloor) noexcept {
    // Branchless: compiles to cmov/csel (or vector blend), and `NaN >= floor` is false
    // so the previous NaN→floor behavior is preserved exactly.
    for (std::size_t i = 0; i < N; ++i) {
        x(i, i) = (x(i, i) >= floor) ? x(i, i) : floor;
    }
}

template <std::size_t N>
constexpr void add_diagonal(Mat<N, N>& x, Scalar value) noexcept {
    for (std::size_t i = 0; i < N; ++i) x(i, i) += value;
}

// General inverse with explicit 1x1, 2x2, and 3x3 formulas. For Kalman updates this is
// used only on the innovation covariance S, whose dimension is the measurement dimension.
template <std::size_t N>
[[nodiscard]] inline bool inverse(const Mat<N, N>& x, Mat<N, N>& inv, Scalar eps = kDefaultEps) noexcept {
    if constexpr (N == 1) {
        const Scalar a = x(0, 0);
        if (std::abs(a) <= eps) return false;
        inv(0, 0) = 1.0 / a;
        return true;
    } else if constexpr (N == 2) {
        const Scalar a = x(0, 0), b = x(0, 1), c = x(1, 0), d = x(1, 1);
        const Scalar det = a * d - b * c;
        if (std::abs(det) <= eps) return false;
        const Scalar id = 1.0 / det;
        inv(0, 0) =  d * id; inv(0, 1) = -b * id;
        inv(1, 0) = -c * id; inv(1, 1) =  a * id;
        return true;
    } else if constexpr (N == 3) {
        const Scalar a = x(0,0), b = x(0,1), c = x(0,2);
        const Scalar d = x(1,0), e = x(1,1), f = x(1,2);
        const Scalar g = x(2,0), h = x(2,1), i = x(2,2);
        const Scalar A00 = e*i - f*h;
        const Scalar A01 = c*h - b*i;
        const Scalar A02 = b*f - c*e;
        const Scalar A10 = f*g - d*i;
        const Scalar A11 = a*i - c*g;
        const Scalar A12 = c*d - a*f;
        const Scalar A20 = d*h - e*g;
        const Scalar A21 = b*g - a*h;
        const Scalar A22 = a*e - b*d;
        const Scalar det = a*A00 + b*A10 + c*A20;
        if (std::abs(det) <= eps) return false;
        const Scalar id = 1.0 / det;
        inv(0,0) = A00*id; inv(0,1) = A01*id; inv(0,2) = A02*id;
        inv(1,0) = A10*id; inv(1,1) = A11*id; inv(1,2) = A12*id;
        inv(2,0) = A20*id; inv(2,1) = A21*id; inv(2,2) = A22*id;
        return true;
    } else {
        Mat<N, 2 * N> aug{};
        for (std::size_t r = 0; r < N; ++r) {
            for (std::size_t c = 0; c < N; ++c) aug(r, c) = x(r, c);
            aug(r, N + r) = 1.0;
        }
        for (std::size_t col = 0; col < N; ++col) {
            std::size_t pivot = col;
            Scalar best = std::abs(aug(col, col));
            for (std::size_t r = col + 1; r < N; ++r) {
                const Scalar v = std::abs(aug(r, col));
                if (v > best) { best = v; pivot = r; }
            }
            if (best <= eps) return false;
            if (pivot != col) {
                for (std::size_t c = 0; c < 2 * N; ++c) std::swap(aug(col, c), aug(pivot, c));
            }
            const Scalar inv_pivot = 1.0 / aug(col, col);
            for (std::size_t c = 0; c < 2 * N; ++c) aug(col, c) *= inv_pivot;
            for (std::size_t r = 0; r < N; ++r) {
                if (r == col) continue;
                const Scalar factor = aug(r, col);
                for (std::size_t c = 0; c < 2 * N; ++c) aug(r, c) -= factor * aug(col, c);
            }
        }
        for (std::size_t r = 0; r < N; ++r) {
            for (std::size_t c = 0; c < N; ++c) inv(r, c) = aug(r, N + c);
        }
        return true;
    }
}

template <std::size_t N>
[[nodiscard]] inline bool cholesky_lower(const Mat<N, N>& input, Mat<N, N>& L, Scalar jitter = kDefaultJitter) noexcept {
    Mat<N, N> x = input;
    symmetrize(x);
    Scalar extra = 0.0;
    for (int attempt = 0; attempt < 7; ++attempt) {
        L.fill(0.0);
        bool ok = true;
        for (std::size_t i = 0; i < N && ok; ++i) {
            for (std::size_t j = 0; j <= i; ++j) {
                Scalar sum = x(i, j);
                if (i == j) sum += extra;
                for (std::size_t k = 0; k < j; ++k) sum -= L(i, k) * L(j, k);
                if (i == j) {
                    if (!(sum > 0.0)) { ok = false; break; }
                    L(i, j) = std::sqrt(sum);
                } else {
                    L(i, j) = sum / L(j, j);
                }
            }
        }
        if (ok) return true;
        extra = (extra == 0.0) ? jitter : extra * 10.0;
    }
    return false;
}

template <std::size_t NX, std::size_t NZ>
struct UpdateStats {
    Vec<NZ> innovation{};
    Mat<NZ, NZ> S{};
    Mat<NX, NZ> K{};
    Scalar nis = 0.0; // normalized innovation squared
    bool ok = true;
};

template <std::size_t NX, std::size_t NZ>
[[nodiscard]] inline Mat<NX, NX> joseph_covariance(const Mat<NX, NX>& P,
                                                    const Mat<NZ, NX>& H,
                                                    const Mat<NZ, NZ>& R,
                                                    const Mat<NX, NZ>& K) noexcept {
    const Mat<NX, NX> I = Mat<NX, NX>::identity();
    const Mat<NX, NX> A = I - K * H;
    Mat<NX, NX> out = A * P * transpose(A) + K * R * transpose(K);
    symmetrize(out);
    floor_diagonal(out);
    return out;
}

template <std::size_t NX, std::size_t NZ>
[[nodiscard]] inline UpdateStats<NX, NZ> linear_correct_with_innovation(Vec<NX>& x,
                                                                         Mat<NX, NX>& P,
                                                                         const Vec<NZ>& innovation,
                                                                         const Mat<NZ, NX>& H,
                                                                         const Mat<NZ, NZ>& R,
                                                                         Scalar eps = kDefaultEps) noexcept {
    UpdateStats<NX, NZ> stats{};
    stats.innovation = innovation;

    if constexpr (NZ == 1) {
        // Scalar fast path: skip building NxN Joseph temporaries; rank-1 P update.
        Vec<NX> h{};
        for (std::size_t i = 0; i < NX; ++i) h[i] = H(0, i);
        const Scalar r = R(0, 0);
        Vec<NX> k{};
        Scalar S = 0.0;
        Scalar nis = 0.0;
        if (!scalar_joseph_update(x, P, innovation[0], h, r, eps, k, S, nis)) {
            // Retry with diagonal jitter on R to mirror the matrix-path recovery.
            if (!scalar_joseph_update(x, P, innovation[0], h, r + kDefaultJitter, eps, k, S, nis)) {
                stats.S(0, 0) = S;
                stats.ok = false;
                return stats;
            }
        }
        for (std::size_t i = 0; i < NX; ++i) stats.K(i, 0) = k[i];
        stats.S(0, 0) = S;
        stats.nis = nis;
        stats.ok = true;
        return stats;
    } else {
        const Mat<NX, NZ> PHt = mul_AB_T(P, H);   // P · Hᵀ without materializing transpose
        stats.S = H * PHt + R;
        symmetrize(stats.S);

        Mat<NZ, NZ> Sinv{};
        if (!inverse(stats.S, Sinv, eps)) {
            add_diagonal(stats.S, kDefaultJitter);
            if (!inverse(stats.S, Sinv, eps)) { stats.ok = false; return stats; }
        }

        stats.K = PHt * Sinv;
        x += stats.K * innovation;

        // Standard form: P ← P − K·(H·P). Since P is symmetric, H·P = (P·Hᵀ)ᵀ, so reuse PHt
        // via mul_AB_T(K, PHt) = K · PHtᵀ. One NX×NZ × NZ×NX matmul replaces the four N×N
        // matmuls of the Joseph expansion.
        P -= mul_AB_T(stats.K, PHt);
        symmetrize(P);
        floor_diagonal(P);
        stats.nis = dot(innovation, Sinv * innovation);
        stats.ok = true;
        return stats;
    }
}

template <std::size_t NX, std::size_t NZ>
[[nodiscard]] inline UpdateStats<NX, NZ> linear_correct(Vec<NX>& x,
                                                         Mat<NX, NX>& P,
                                                         const Vec<NZ>& z,
                                                         const Mat<NZ, NX>& H,
                                                         const Mat<NZ, NZ>& R,
                                                         Scalar eps = kDefaultEps) noexcept {
    return linear_correct_with_innovation(x, P, z - H * x, H, R, eps);
}

// One backward step of the Rauch-Tung-Striebel smoother for a linear/Gaussian model.
// Given:
//   x_filtered, P_filtered:       filtered estimate at time t (posterior, after correct)
//   x_pred_next, P_pred_next:     predicted estimate at time t+1 (prior, after predict)
//   x_smoothed_next, P_smoothed_next: smoothed estimate at time t+1 (already computed)
//   F:                            transition matrix used for the t -> t+1 predict
// Computes smoothed estimate at time t into x_smoothed_out, P_smoothed_out using:
//   J     = P_filtered · Fᵀ · (P_pred_next)⁻¹
//   x_t^s = x_filtered + J · (x_smoothed_next − x_pred_next)
//   P_t^s = P_filtered + J · (P_smoothed_next − P_pred_next) · Jᵀ
// All intermediate products are buffered before writing the output, so it is safe for
// the *_smoothed_out aliases to point at the same storage as the *_smoothed_next inputs
// during an in-place backward sweep.
// Returns false if P_pred_next is singular; in that case the outputs are not modified.
template <std::size_t N>
[[nodiscard]] inline bool rts_smooth_step(const Vec<N>& x_filtered,
                                           const Mat<N, N>& P_filtered,
                                           const Vec<N>& x_pred_next,
                                           const Mat<N, N>& P_pred_next,
                                           const Vec<N>& x_smoothed_next,
                                           const Mat<N, N>& P_smoothed_next,
                                           const Mat<N, N>& F,
                                           Vec<N>& x_smoothed_out,
                                           Mat<N, N>& P_smoothed_out,
                                           Scalar eps = kDefaultEps) noexcept {
    Mat<N, N> P_pred_inv{};
    if (!inverse(P_pred_next, P_pred_inv, eps)) return false;

    // J = P_filtered · Fᵀ · P_pred_next⁻¹
    const Mat<N, N> PFt = mul_AB_T(P_filtered, F);
    const Mat<N, N> J = PFt * P_pred_inv;

    const Vec<N> dx = x_smoothed_next - x_pred_next;
    const Vec<N> Jdx = J * dx;

    const Mat<N, N> dP = P_smoothed_next - P_pred_next;
    const Mat<N, N> JdP = J * dP;
    const Mat<N, N> JdPJt = mul_AB_T(JdP, J);

    // Now safe to write outputs (all dependencies on *_smoothed_next have been buffered).
    x_smoothed_out = x_filtered + Jdx;
    P_smoothed_out = P_filtered + JdPJt;
    symmetrize(P_smoothed_out);
    floor_diagonal(P_smoothed_out);
    return true;
}

// Scalar-measurement covariance update applied in place. Used both by the standalone
// scalar_correct path and by linear_correct_with_innovation when NZ == 1.
//
// Given v = P·h, S = hᵀ·v + r, k = v / S, the explicit Joseph form
//   P_new = (I − k hᵀ) P (I − h kᵀ) + r k kᵀ
// reduces algebraically (since k = v/S) to the single rank-1 update
//   P_new = P − k vᵀ
// because the cross terms collapse: −k vᵀ − v kᵀ + (hᵀ v) k kᵀ + r k kᵀ
//                                = −k vᵀ − v kᵀ + S k kᵀ
//                                = −k vᵀ − v kᵀ + v vᵀ / S
//                                = −v vᵀ / S = −k vᵀ.
// This is also identical to the standard form P − K H P for K = v/S.
//
// Returns false (and leaves P untouched) if S is effectively zero.
template <std::size_t NX>
[[nodiscard]] inline bool scalar_joseph_update(Vec<NX>& x,
                                                Mat<NX, NX>& P,
                                                Scalar y,
                                                const Vec<NX>& h,
                                                Scalar r,
                                                Scalar eps,
                                                Vec<NX>& k_out,
                                                Scalar& S_out,
                                                Scalar& nis_out) noexcept {
    Vec<NX> v{};
    for (std::size_t i = 0; i < NX; ++i) {
        Scalar s = 0.0;
        for (std::size_t j = 0; j < NX; ++j) s += P(i, j) * h[j];
        v[i] = s;
    }
    Scalar S = r;
    for (std::size_t i = 0; i < NX; ++i) S += h[i] * v[i];
    S_out = S;
    if (std::abs(S) <= eps) return false;

    const Scalar invS = 1.0 / S;
    for (std::size_t i = 0; i < NX; ++i) {
        const Scalar ki = v[i] * invS;
        k_out[i] = ki;
        x[i] += ki * y;
    }
    for (std::size_t i = 0; i < NX; ++i) {
        const Scalar ki = k_out[i];
        for (std::size_t j = 0; j < NX; ++j) {
            P(i, j) -= ki * v[j];
        }
    }
    symmetrize(P);
    floor_diagonal(P);
    nis_out = y * y * invS;
    return true;
}

// Scalar-measurement hot path: avoids building 1xN temporary matrices during gain computation.
// Covariance is updated via the rank-1 form above (algebraically equivalent to Joseph).
template <std::size_t NX>
[[nodiscard]] inline UpdateStats<NX, 1> scalar_correct(Vec<NX>& x,
                                                        Mat<NX, NX>& P,
                                                        Scalar z,
                                                        const Vec<NX>& h,
                                                        Scalar r,
                                                        Scalar eps = kDefaultEps) noexcept {
    UpdateStats<NX, 1> stats{};
    const Scalar y = z - dot(h, x);
    stats.innovation[0] = y;
    Vec<NX> k{};
    Scalar S = 0.0;
    Scalar nis = 0.0;
    if (!scalar_joseph_update(x, P, y, h, r, eps, k, S, nis)) {
        stats.S(0, 0) = S;
        stats.ok = false;
        return stats;
    }
    for (std::size_t i = 0; i < NX; ++i) stats.K(i, 0) = k[i];
    stats.S(0, 0) = S;
    stats.nis = nis;
    stats.ok = true;
    return stats;
}

template <std::size_t NX, std::size_t NZ>
class LinearKalmanFilter {
public:
    Vec<NX> x{};
    Mat<NX, NX> P = Mat<NX, NX>::identity();
    Mat<NX, NX> F = Mat<NX, NX>::identity();
    Mat<NX, NX> Q{};
    Mat<NZ, NX> H{};
    Mat<NZ, NZ> R = Mat<NZ, NZ>::identity();
    UpdateStats<NX, NZ> last{};

    inline void predict() noexcept {
        x = F * x;
        const Mat<NX, NX> FP = F * P;
        P = mul_AB_T(FP, F) + Q;
        symmetrize(P);
        floor_diagonal(P);
    }

    template <std::size_t NU>
    inline void predict(const Mat<NX, NU>& B, const Vec<NU>& u) noexcept {
        x = F * x + B * u;
        const Mat<NX, NX> FP = F * P;
        P = mul_AB_T(FP, F) + Q;
        symmetrize(P);
        floor_diagonal(P);
    }

    [[nodiscard]] inline UpdateStats<NX, NZ> correct(const Vec<NZ>& z, Scalar eps = kDefaultEps) noexcept {
        last = linear_correct(x, P, z, H, R, eps);
        return last;
    }

    [[nodiscard]] inline UpdateStats<NX, NZ> update(const Vec<NZ>& z, Scalar eps = kDefaultEps) noexcept {
        predict();
        return correct(z, eps);
    }

    inline void set_delta_t(Scalar dt) noexcept requires (NX == 2 * NZ || NX == 3 * NZ) {
        if constexpr (NX == 2 * NZ) {
            for (std::size_t i = 0; i < NZ; ++i) F(i, NZ + i) = dt;
        } else {
            for (std::size_t i = 0; i < NZ; ++i) {
                F(i, NZ + i) = dt;
                F(i, 2 * NZ + i) = 0.5 * dt * dt;
                F(NZ + i, 2 * NZ + i) = dt;
            }
        }
    }

    // Predict using the kinematic structure of F directly (constant-velocity or
    // constant-acceleration), avoiding the generic F·P·Fᵀ + Q matmul chain.
    // F is also updated via set_delta_t so callers observing kf.F see a consistent
    // delta_t after this call.
    inline void predict_kinematic(Scalar dt) noexcept requires (NX == 2 * NZ || NX == 3 * NZ) {
        set_delta_t(dt);

        if constexpr (NX == 2 * NZ) {
            // State: x_pos += dt · x_vel; x_vel unchanged.
            for (std::size_t i = 0; i < NZ; ++i) x[i] += dt * x[NZ + i];

            // F·P·Fᵀ via two rank-NZ shears (the second uses values already overwritten
            // by the first — that is intentional: it implements the right multiplication).
            for (std::size_t i = 0; i < NZ; ++i) {
                for (std::size_t j = 0; j < NX; ++j) P(i, j) += dt * P(NZ + i, j);
            }
            for (std::size_t i = 0; i < NX; ++i) {
                for (std::size_t j = 0; j < NZ; ++j) P(i, j) += dt * P(i, NZ + j);
            }
        } else {
            const Scalar dt2_half = 0.5 * dt * dt;

            // State (compute in dependency order so x_vel and x_acc reads are pre-update):
            for (std::size_t i = 0; i < NZ; ++i) {
                x[i] += dt * x[NZ + i] + dt2_half * x[2 * NZ + i];
                x[NZ + i] += dt * x[2 * NZ + i];
            }

            // Row pass (F·P, top rows depend on bottom rows so iterate top-to-bottom).
            for (std::size_t i = 0; i < NZ; ++i) {
                for (std::size_t j = 0; j < NX; ++j) {
                    P(i, j) += dt * P(NZ + i, j) + dt2_half * P(2 * NZ + i, j);
                }
            }
            for (std::size_t i = 0; i < NZ; ++i) {
                for (std::size_t j = 0; j < NX; ++j) {
                    P(NZ + i, j) += dt * P(2 * NZ + i, j);
                }
            }
            // Column pass ((F·P)·Fᵀ, left columns depend on right columns).
            for (std::size_t i = 0; i < NX; ++i) {
                for (std::size_t j = 0; j < NZ; ++j) {
                    P(i, j) += dt * P(i, NZ + j) + dt2_half * P(i, 2 * NZ + j);
                }
            }
            for (std::size_t i = 0; i < NX; ++i) {
                for (std::size_t j = 0; j < NZ; ++j) {
                    P(i, NZ + j) += dt * P(i, 2 * NZ + j);
                }
            }
        }

        P += Q;
        symmetrize(P);
        floor_diagonal(P);
    }

    [[nodiscard]] inline UpdateStats<NX, NZ> update(const Vec<NZ>& z, Scalar dt, Scalar eps) noexcept requires (NX == 2 * NZ || NX == 3 * NZ) {
        predict_kinematic(dt);
        return correct(z, eps);
    }

    [[nodiscard]] inline UpdateStats<NX, 1> correct_scalar(Scalar z, const Vec<NX>& h, Scalar r, Scalar eps = kDefaultEps) noexcept requires (NZ == 1) {
        auto stats = scalar_correct(x, P, z, h, r, eps);
        last.innovation[0] = stats.innovation[0];
        last.S(0, 0) = stats.S(0, 0);
        for (std::size_t i = 0; i < NX; ++i) last.K(i, 0) = stats.K(i, 0);
        last.nis = stats.nis;
        last.ok = stats.ok;
        return stats;
    }
};

template <std::size_t NX, std::size_t NZ>
class ExtendedKalmanFilter {
public:
    Vec<NX> x{};
    Mat<NX, NX> P = Mat<NX, NX>::identity();
    Mat<NX, NX> Q{};
    Mat<NZ, NZ> R = Mat<NZ, NZ>::identity();
    UpdateStats<NX, NZ> last{};

    template <class TransitionFn, class TransitionJacobianFn>
    inline void predict(TransitionFn&& transition, TransitionJacobianFn&& jacobian) {
        const Vec<NX> x0 = x;
        const Mat<NX, NX> A = jacobian(x0);
        x = transition(x0);
        const Mat<NX, NX> AP = A * P;
        P = mul_AB_T(AP, A) + Q;
        symmetrize(P);
        floor_diagonal(P);
    }

    template <class MeasurementFn, class MeasurementJacobianFn>
    [[nodiscard]] inline UpdateStats<NX, NZ> correct(const Vec<NZ>& z,
                                                      MeasurementFn&& measurement,
                                                      MeasurementJacobianFn&& jacobian,
                                                      Scalar eps = kDefaultEps) {
        const Vec<NZ> zhat = measurement(x);
        const Mat<NZ, NX> H = jacobian(x);
        last = linear_correct_with_innovation(x, P, z - zhat, H, R, eps);
        return last;
    }
};

template <std::size_t NX, std::size_t NZ>
class IteratedExtendedKalmanFilter {
public:
    Vec<NX> x{};
    Mat<NX, NX> P = Mat<NX, NX>::identity();
    Mat<NX, NX> Q{};
    Mat<NZ, NZ> R = Mat<NZ, NZ>::identity();
    UpdateStats<NX, NZ> last{};

    template <class TransitionFn, class TransitionJacobianFn>
    inline void predict(TransitionFn&& transition, TransitionJacobianFn&& jacobian) {
        const Vec<NX> x0 = x;
        const Mat<NX, NX> A = jacobian(x0);
        x = transition(x0);
        const Mat<NX, NX> AP = A * P;
        P = mul_AB_T(AP, A) + Q;
        symmetrize(P);
        floor_diagonal(P);
    }

    template <class MeasurementFn, class MeasurementJacobianFn>
    [[nodiscard]] inline UpdateStats<NX, NZ> correct(const Vec<NZ>& z,
                                                      MeasurementFn&& measurement,
                                                      MeasurementJacobianFn&& jacobian,
                                                      std::size_t max_iterations = 5,
                                                      Scalar tolerance = 1.0e-10,
                                                      Scalar eps = kDefaultEps) {
        const Vec<NX> x_prior = x;
        const Mat<NX, NX> P_prior = P;
        Vec<NX> xi = x_prior;
        Mat<NZ, NX> Hlast{};
        UpdateStats<NX, NZ> stats{};

        for (std::size_t iter = 0; iter < max_iterations; ++iter) {
            const Vec<NZ> zhat = measurement(xi);
            const Mat<NZ, NX> H = jacobian(xi);
            Hlast = H;

            const Vec<NZ> innovation = z - zhat + H * (xi - x_prior);
            const Mat<NX, NZ> PHt = mul_AB_T(P_prior, H);
            stats.S = H * PHt + R;
            symmetrize(stats.S);

            Mat<NZ, NZ> Sinv{};
            if (!inverse(stats.S, Sinv, eps)) { stats.ok = false; return stats; }
            stats.K = PHt * Sinv;

            const Vec<NX> x_next = x_prior + stats.K * innovation;
            const Scalar step = norm(x_next - xi);
            xi = x_next;
            stats.innovation = z - measurement(xi);
            stats.nis = dot(stats.innovation, Sinv * stats.innovation);
            stats.ok = true;
            if (step <= tolerance) break;
        }

        x = xi;
        P = joseph_covariance(P_prior, Hlast, R, stats.K);
        last = stats;
        return last;
    }
};

struct UnscentedParameters {
    Scalar alpha = 1.0e-3;
    Scalar beta = 2.0;
    Scalar kappa = 0.0;
};

template <std::size_t NX, std::size_t NZ>
class UnscentedKalmanFilter {
public:
    static constexpr std::size_t SigmaCount = 2 * NX + 1;

    Vec<NX> x{};
    Mat<NX, NX> P = Mat<NX, NX>::identity();
    Mat<NX, NX> Q{};
    Mat<NZ, NZ> R = Mat<NZ, NZ>::identity();
    UnscentedParameters params{};
    UpdateStats<NX, NZ> last{};

    [[nodiscard]] inline Scalar lambda() const noexcept {
        return params.alpha * params.alpha * (static_cast<Scalar>(NX) + params.kappa) - static_cast<Scalar>(NX);
    }

    [[nodiscard]] inline Scalar scale() const noexcept { return static_cast<Scalar>(NX) + lambda(); }
    [[nodiscard]] inline Scalar wm(std::size_t i) const noexcept { return i == 0 ? lambda() / scale() : 1.0 / (2.0 * scale()); }
    [[nodiscard]] inline Scalar wc(std::size_t i) const noexcept { return i == 0 ? wm(0) + 1.0 - params.alpha * params.alpha + params.beta : wm(i); }

    [[nodiscard]] inline bool sigma_points(std::array<Vec<NX>, SigmaCount>& sigma, Scalar jitter = kDefaultJitter) const noexcept {
        const Scalar c = scale();
        if (!(c > 0.0)) return false;
        Mat<NX, NX> L{};
        if (!cholesky_lower(P * c, L, jitter)) return false;
        sigma[0] = x;
        for (std::size_t col = 0; col < NX; ++col) {
            Vec<NX> delta{};
            for (std::size_t row = 0; row < NX; ++row) delta[row] = L(row, col);
            sigma[1 + col] = x + delta;
            sigma[1 + NX + col] = x - delta;
        }
        return true;
    }

    template <class TransitionFn>
    [[nodiscard]] inline bool predict(TransitionFn&& transition, Scalar jitter = kDefaultJitter) {
        std::array<Vec<NX>, SigmaCount> sigma{};
        if (!sigma_points(sigma, jitter)) return false;

        std::array<Vec<NX>, SigmaCount> propagated{};
        Vec<NX> mean{};
        for (std::size_t i = 0; i < SigmaCount; ++i) {
            propagated[i] = transition(sigma[i]);
            mean += propagated[i] * wm(i);
        }

        Mat<NX, NX> cov = Q;
        for (std::size_t i = 0; i < SigmaCount; ++i) {
            const Vec<NX> dx = propagated[i] - mean;
            cov += outer(dx, dx) * wc(i);
        }
        x = mean;
        P = cov;
        symmetrize(P);
        floor_diagonal(P);
        return true;
    }

    template <class MeasurementFn>
    [[nodiscard]] inline UpdateStats<NX, NZ> correct(const Vec<NZ>& z,
                                                      MeasurementFn&& measurement,
                                                      Scalar jitter = kDefaultJitter,
                                                      Scalar eps = kDefaultEps) {
        UpdateStats<NX, NZ> stats{};
        std::array<Vec<NX>, SigmaCount> sigma{};
        if (!sigma_points(sigma, jitter)) { stats.ok = false; return stats; }

        std::array<Vec<NZ>, SigmaCount> zsigma{};
        Vec<NZ> zmean{};
        for (std::size_t i = 0; i < SigmaCount; ++i) {
            zsigma[i] = measurement(sigma[i]);
            zmean += zsigma[i] * wm(i);
        }

        Mat<NZ, NZ> S = R;
        Mat<NX, NZ> Pxz{};
        for (std::size_t i = 0; i < SigmaCount; ++i) {
            const Vec<NX> dx = sigma[i] - x;
            const Vec<NZ> dz = zsigma[i] - zmean;
            S += outer(dz, dz) * wc(i);
            Pxz += outer(dx, dz) * wc(i);
        }
        symmetrize(S);

        Mat<NZ, NZ> Sinv{};
        if (!inverse(S, Sinv, eps)) { stats.ok = false; return stats; }
        stats.K = Pxz * Sinv;
        stats.innovation = z - zmean;
        stats.S = S;
        x += stats.K * stats.innovation;
        const Mat<NX, NZ> KS = stats.K * S;
        P -= mul_AB_T(KS, stats.K);
        symmetrize(P);
        floor_diagonal(P);
        stats.nis = dot(stats.innovation, Sinv * stats.innovation);
        stats.ok = true;
        last = stats;
        return stats;
    }
};

// Generic invariant-error EKF shell. The StateT can be a Lie-group-like type; the caller
// provides propagation, measurement, innovation, and retraction semantics.
template <std::size_t ErrorN, std::size_t NZ, class StateT>
class InvariantExtendedKalmanFilter {
public:
    StateT state{};
    Mat<ErrorN, ErrorN> P = Mat<ErrorN, ErrorN>::identity();
    Mat<ErrorN, ErrorN> Q{};
    Mat<NZ, NZ> R = Mat<NZ, NZ>::identity();
    UpdateStats<ErrorN, NZ> last{};

    template <class PropagateFn, class ErrorTransitionFn>
    inline void predict(PropagateFn&& propagate, ErrorTransitionFn&& error_transition) {
        const StateT before = state;
        state = propagate(state);
        const Mat<ErrorN, ErrorN> A = error_transition(before, state);
        const Mat<ErrorN, ErrorN> AP = A * P;
        P = mul_AB_T(AP, A) + Q;
        symmetrize(P);
        floor_diagonal(P);
    }

    template <class MeasurementFn, class ErrorJacobianFn, class InnovationFn, class RetractFn>
    [[nodiscard]] inline UpdateStats<ErrorN, NZ> correct(const Vec<NZ>& z,
                                                          MeasurementFn&& measurement,
                                                          ErrorJacobianFn&& error_jacobian,
                                                          InnovationFn&& innovation,
                                                          RetractFn&& retract,
                                                          Scalar eps = kDefaultEps) {
        UpdateStats<ErrorN, NZ> stats{};
        const Vec<NZ> zhat = measurement(state);
        const Mat<NZ, ErrorN> H = error_jacobian(state);
        stats.innovation = innovation(z, zhat);
        const Mat<ErrorN, NZ> PHt = mul_AB_T(P, H);
        stats.S = H * PHt + R;
        symmetrize(stats.S);

        Mat<NZ, NZ> Sinv{};
        if (!inverse(stats.S, Sinv, eps)) { stats.ok = false; return stats; }
        stats.K = PHt * Sinv;
        const Vec<ErrorN> delta = stats.K * stats.innovation;
        state = retract(state, delta);
        P = joseph_covariance(P, H, R, stats.K);
        stats.nis = dot(stats.innovation, Sinv * stats.innovation);
        stats.ok = true;
        last = stats;
        return stats;
    }
};

template <std::size_t NX, std::size_t NZ, std::size_t EnsembleSize>
class EnsembleKalmanFilter {
public:
    static_assert(EnsembleSize >= 2);

    std::array<Vec<NX>, EnsembleSize> members{};
    Vec<NX> x{};
    Mat<NX, NX> P{};
    Mat<NZ, NZ> R = Mat<NZ, NZ>::identity();
    UpdateStats<NX, NZ> last{};

    inline void recompute_mean_covariance() noexcept {
        x.fill(0.0);
        const Scalar inv_n = 1.0 / static_cast<Scalar>(EnsembleSize);
        for (const auto& m : members) x += m * inv_n;

        P.fill(0.0);
        const Scalar inv_nm1 = 1.0 / static_cast<Scalar>(EnsembleSize - 1);
        for (const auto& m : members) {
            const Vec<NX> dx = m - x;
            P += outer(dx, dx) * inv_nm1;
        }
        symmetrize(P);
        floor_diagonal(P);
    }

    template <class TransitionFn>
    inline void predict(TransitionFn&& transition) {
        for (auto& m : members) m = transition(m);
        recompute_mean_covariance();
    }

    template <class TransitionFn, class ProcessNoiseFn>
    inline void predict(TransitionFn&& transition, ProcessNoiseFn&& process_noise) {
        for (std::size_t i = 0; i < EnsembleSize; ++i) members[i] = transition(members[i]) + process_noise(i);
        recompute_mean_covariance();
    }

    template <class MeasurementFn, class ObservationNoiseFn>
    [[nodiscard]] inline UpdateStats<NX, NZ> correct(const Vec<NZ>& z,
                                                      MeasurementFn&& measurement,
                                                      ObservationNoiseFn&& observation_noise,
                                                      Scalar eps = kDefaultEps) {
        UpdateStats<NX, NZ> stats{};
        std::array<Vec<NZ>, EnsembleSize> y_members{};
        Vec<NZ> y_mean{};
        const Scalar inv_n = 1.0 / static_cast<Scalar>(EnsembleSize);
        const Scalar inv_nm1 = 1.0 / static_cast<Scalar>(EnsembleSize - 1);

        for (std::size_t i = 0; i < EnsembleSize; ++i) {
            y_members[i] = measurement(members[i]);
            y_mean += y_members[i] * inv_n;
        }

        Mat<NX, NZ> Pxy{};
        Mat<NZ, NZ> Pyy = R;
        for (std::size_t i = 0; i < EnsembleSize; ++i) {
            const Vec<NX> dx = members[i] - x;
            const Vec<NZ> dy = y_members[i] - y_mean;
            Pxy += outer(dx, dy) * inv_nm1;
            Pyy += outer(dy, dy) * inv_nm1;
        }
        symmetrize(Pyy);

        Mat<NZ, NZ> PyyInv{};
        if (!inverse(Pyy, PyyInv, eps)) { stats.ok = false; return stats; }
        stats.K = Pxy * PyyInv;
        stats.innovation = z - y_mean;
        stats.S = Pyy;

        for (std::size_t i = 0; i < EnsembleSize; ++i) {
            const Vec<NZ> zi = z + observation_noise(i);
            members[i] += stats.K * (zi - y_members[i]);
        }

        recompute_mean_covariance();
        stats.nis = dot(stats.innovation, PyyInv * stats.innovation);
        stats.ok = true;
        last = stats;
        return stats;
    }

    template <class MeasurementFn>
    [[nodiscard]] inline UpdateStats<NX, NZ> correct_deterministic(const Vec<NZ>& z,
                                                                    MeasurementFn&& measurement,
                                                                    Scalar eps = kDefaultEps) {
        auto zero_noise = [](std::size_t) noexcept { return Vec<NZ>{}; };
        return correct(z, std::forward<MeasurementFn>(measurement), zero_noise, eps);
    }
};

struct AdaptiveOptions {
    Scalar forgetting = 0.98;                 // 1.0 = no adaptation, lower = faster adaptation
    Scalar min_variance = 1.0e-12;
    bool adapt_R = true;
    bool diagonal_R_only = true;
    bool adapt_Q_from_state_correction = false; // heuristic; disabled by default
};

template <std::size_t NX, std::size_t NZ>
class AdaptiveKalmanFilter {
public:
    LinearKalmanFilter<NX, NZ> kf{};
    AdaptiveOptions options{};
    Mat<NZ, NZ> innovation_covariance_ema{};
    bool initialized = false;

    inline void predict() noexcept { kf.predict(); }

    [[nodiscard]] inline UpdateStats<NX, NZ> correct(const Vec<NZ>& z, Scalar eps = kDefaultEps) noexcept {
        const Vec<NZ> innovation = z - kf.H * kf.x;
        const Mat<NX, NZ> PHt = mul_AB_T(kf.P, kf.H);
        Mat<NZ, NZ> predicted_measurement_cov = kf.H * PHt;
        symmetrize(predicted_measurement_cov);

        const Mat<NZ, NZ> yyT = outer(innovation, innovation);
        const Scalar f = std::clamp(options.forgetting, Scalar{0.0}, Scalar{1.0});
        if (!initialized) {
            innovation_covariance_ema = yyT;
            initialized = true;
        } else {
            innovation_covariance_ema = innovation_covariance_ema * f + yyT * (1.0 - f);
        }

        if (options.adapt_R) {
            Mat<NZ, NZ> candidate_R = innovation_covariance_ema - predicted_measurement_cov;
            symmetrize(candidate_R);
            floor_diagonal(candidate_R, options.min_variance);
            if (options.diagonal_R_only) {
                Mat<NZ, NZ> diag_R{};
                for (std::size_t i = 0; i < NZ; ++i) diag_R(i, i) = candidate_R(i, i);
                candidate_R = diag_R;
            }
            kf.R = kf.R * f + candidate_R * (1.0 - f);
            symmetrize(kf.R);
            floor_diagonal(kf.R, options.min_variance);
        }

        const Vec<NX> x_before = kf.x;
        auto stats = kf.correct(z, eps);
        if (options.adapt_Q_from_state_correction && stats.ok) {
            Mat<NX, NX> candidate_Q = outer(kf.x - x_before, kf.x - x_before);
            floor_diagonal(candidate_Q, options.min_variance);
            kf.Q = kf.Q * f + candidate_Q * (1.0 - f);
            symmetrize(kf.Q);
        }
        return stats;
    }

    [[nodiscard]] inline UpdateStats<NX, NZ> update(const Vec<NZ>& z, Scalar eps = kDefaultEps) noexcept {
        predict();
        return correct(z, eps);
    }
};

[[nodiscard]] inline LinearKalmanFilter<1, 1> make_scalar_random_walk(Scalar initial_value,
                                                                       Scalar initial_variance,
                                                                       Scalar process_variance,
                                                                       Scalar measurement_variance) noexcept {
    LinearKalmanFilter<1, 1> kf{};
    kf.x = Vec<1>{initial_value};
    kf.P = Mat<1, 1>{initial_variance};
    kf.F = Mat<1, 1>{1.0};
    kf.Q = Mat<1, 1>{process_variance};
    kf.H = Mat<1, 1>{1.0};
    kf.R = Mat<1, 1>{measurement_variance};
    return kf;
}

[[nodiscard]] inline LinearKalmanFilter<2, 1> make_constant_velocity_1d(Scalar initial_position,
                                                                         Scalar initial_velocity,
                                                                         Scalar dt,
                                                                         Scalar position_variance,
                                                                         Scalar velocity_variance,
                                                                         Scalar process_position_variance,
                                                                         Scalar process_velocity_variance,
                                                                         Scalar measurement_variance) noexcept {
    LinearKalmanFilter<2, 1> kf{};
    kf.x = Vec<2>{initial_position, initial_velocity};
    kf.P = Mat<2, 2>{position_variance, 0.0, 0.0, velocity_variance};
    kf.F = Mat<2, 2>{1.0, dt, 0.0, 1.0};
    kf.Q = Mat<2, 2>{process_position_variance, 0.0, 0.0, process_velocity_variance};
    kf.H = Mat<1, 2>{1.0, 0.0};
    kf.R = Mat<1, 1>{measurement_variance};
    return kf;
}

[[nodiscard]] inline LinearKalmanFilter<3, 1> make_constant_acceleration_1d(Scalar initial_position,
                                                                             Scalar initial_velocity,
                                                                             Scalar initial_acceleration,
                                                                             Scalar dt,
                                                                             Scalar position_variance,
                                                                             Scalar velocity_variance,
                                                                             Scalar acceleration_variance,
                                                                             Scalar process_position_variance,
                                                                             Scalar process_velocity_variance,
                                                                             Scalar process_acceleration_variance,
                                                                             Scalar measurement_variance) noexcept {
    const Scalar dt2 = 0.5 * dt * dt;
    LinearKalmanFilter<3, 1> kf{};
    kf.x = Vec<3>{initial_position, initial_velocity, initial_acceleration};
    kf.P = Mat<3, 3>{position_variance, 0.0, 0.0,
                     0.0, velocity_variance, 0.0,
                     0.0, 0.0, acceleration_variance};
    kf.F = Mat<3, 3>{1.0, dt, dt2,
                     0.0, 1.0, dt,
                     0.0, 0.0, 1.0};
    kf.Q = Mat<3, 3>{process_position_variance, 0.0, 0.0,
                     0.0, process_velocity_variance, 0.0,
                     0.0, 0.0, process_acceleration_variance};
    kf.H = Mat<1, 3>{1.0, 0.0, 0.0};
    kf.R = Mat<1, 1>{measurement_variance};
    return kf;
}

using ScalarKalmanFilter = LinearKalmanFilter<1, 1>;
using LocalLinearTrendFilter = LinearKalmanFilter<2, 1>;
using ConstantAccelerationFilter = LinearKalmanFilter<3, 1>;

} // namespace kalman
