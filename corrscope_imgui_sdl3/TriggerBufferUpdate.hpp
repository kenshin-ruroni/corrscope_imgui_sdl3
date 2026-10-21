#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <cassert>
#include <stdexcept>
#include <optional>

using f32 = float;

constexpr f32 MIN_AMPLITUDE = 1e-7f;

inline void normalize_buffer_simd(std::vector<f32>& buffer) {
    if (buffer.empty()) return;

    f32* ptr = buffer.data();
    size_t size = buffer.size();
    f32 abs_max_val = MIN_AMPLITUDE;

    #pragma omp simd reduction(max:abs_max_val)
    for (size_t i = 0; i < size; ++i) {
        f32 abs_val = std::abs(ptr[i]);
        if (abs_val > abs_max_val) {
            abs_max_val = abs_val;
        }
    }

    f32 inv_max = 1.0f / abs_max_val;
    #pragma omp simd
    for (size_t i = 0; i < size; ++i) {
        ptr[i] *= inv_max;
    }
}

inline std::vector<f32> lerp_simd(const std::vector<f32>& a, const std::vector<f32>& b, f32 t) {
    assert(a.size() == b.size());
    size_t size = a.size();
    std::vector<f32> result(size);

    const f32* a_ptr = a.data();
    const f32* b_ptr = b.data();
    f32* res_ptr = result.data();

    #pragma omp simd
    for (size_t i = 0; i < size; ++i) {
        res_ptr[i] = a_ptr[i] + t * (b_ptr[i] - a_ptr[i]);
    }

    return result;
}

inline std::vector<f32> generate_gaussian_window(size_t n, f32 std_dev) {
    std::vector<f32> window(n);
    if (std_dev <= 0.0f) {
        std::fill(window.begin(), window.end(), 0.0f);
        return window;
    }

    f32* win_ptr = window.data();
    f32 mid = static_cast<f32>(n - 1) / 2.0f;
    f32 inv_two_std_sq = -1.0f / (2.0f * std_dev * std_dev);

    #pragma omp simd
    for (size_t i = 0; i < n; ++i) {
        f32 x = static_cast<f32>(i) - mid;
        win_ptr[i] = std::exp(x * x * inv_two_std_sq);
    }

    return window;
}
