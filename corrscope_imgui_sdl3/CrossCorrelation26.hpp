#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <optional>
#include <ranges>
#include <cassert>
#include "TriggerBufferUpdate.hpp"

using f32 = float;

struct TriggerResult {
    int trigger;
    std::optional<f32> freq_estimate;
};

inline std::vector<f32> correlate_valid(const std::vector<f32>& data, const std::vector<f32>& kernel) {
    if (data.size() < kernel.size()) return {};
    
    int corr_nsamp = data.size() - kernel.size() + 1;
    std::vector<f32> out_corr(corr_nsamp);

    auto windows = data | std::views::slide(kernel.size());
    
    int i = 0;
    for (auto window : windows) {
        auto zipped = std::views::zip(window, kernel);
        out_corr[i++] = std::accumulate(zipped.begin(), zipped.end(), 0.0f, 
            [](f32 sum, const auto& pair) {
                return sum + (std::get<0>(pair) * std::get<1>(pair));
            });
    }
    return out_corr;
}

inline int find_peak(const std::vector<f32>& corr, const std::vector<f32>& peaks, std::optional<int> radius) {
    assert(corr.size() == peaks.size());
    int corr_nsamp = corr.size();

    int mid = corr_nsamp / 2;
    int left = 0;
    int right = corr_nsamp;

    if (radius.has_value()) {
        left = std::max(mid - radius.value(), 0);
        right = std::min(mid + radius.value() + 1, corr_nsamp);
    }

    auto sub_corr = std::ranges::subrange(corr.begin() + left, corr.begin() + right);
    f32 min_corr = std::ranges::min(sub_corr);

    std::vector<f32> corr_local = corr;

    for (int i : std::views::iota(left, right - 1)) {
        if (peaks[i] < peaks[i + 1]) corr_local[i] = min_corr;
    }
    for (int i : std::views::iota(left + 1, right)) {
        if (peaks[i] < peaks[i - 1]) corr_local[i] = min_corr;
    }

    if (left < corr_nsamp) corr_local[left] = min_corr;
    if (right - 1 >= 0) corr_local[right - 1] = min_corr;

    auto local_sub = std::ranges::subrange(corr_local.begin() + left, corr_local.begin() + right);
    auto max_it = std::ranges::max_element(local_sub);
    int peak_corr_idx = std::distance(corr_local.begin(), max_it);

    if (corr_local[peak_corr_idx] == min_corr) {
        return mid;
    }
    
    return peak_corr_idx;
}

class CorrelationTrigger {
private:
    int A;
    int B;
    int _tsamp;
    int _stride;
    int _smp_per_frame;
    int _trigger_diameter;
    int _prev_trigger = 0;
    
    f32 edge_strength;
    f32 buffer_strength;
    f32 responsiveness;
    f32 buffer_falloff;

    std::vector<f32> _corr_buffer;   
    std::vector<f32> slope_finder;   
    std::vector<f32> _prev_window;

    f32 calc_buffer_std(f32 period) const {
        return period * buffer_falloff;
    }

public:
    struct PerFrameCache {
        std::optional<f32> result_mean;
        std::optional<f32> period;
    };

    CorrelationTrigger(int tsamp, int stride, int smp_per_frame, f32 trigger_diameter_pct, f32 edge_str, f32 buf_str)
        : _tsamp(tsamp), _stride(stride), _smp_per_frame(smp_per_frame), edge_strength(edge_str), buffer_strength(buf_str) {
        
        A = B = _tsamp / 2;
        int kernel_size = A + B;
        _trigger_diameter = static_cast<int>(kernel_size * trigger_diameter_pct);

        responsiveness = 0.1f;
        buffer_falloff = 0.5f;

        _corr_buffer.assign(kernel_size, 0.0f);
        slope_finder.assign(kernel_size, 0.0f); 
        _prev_window.assign(kernel_size, 0.0f);
    }

    TriggerResult get_trigger(const std::vector<f32>& raw_wave, int pos, std::optional<int> trigger_radius) {
        int kernel_size = A + B;
        int data_nsubsmp = A + _trigger_diameter + B;
        int corr_nsamp = _trigger_diameter + 1;

        int trigger_begin = std::max(pos - _smp_per_frame, pos - (_trigger_diameter / 2));
        int data_begin = trigger_begin - (_stride * A);

        std::vector<f32> data(data_nsubsmp, 0.0f);
        for (int i : std::views::iota(0, data_nsubsmp)) {
            int idx = data_begin + (i * _stride);
            if (idx >= 0 && idx < static_cast<int>(raw_wave.size())) {
                data[i] = raw_wave[idx];
            }
        }

        f32 sum = std::ranges::fold_left(data, 0.0f, std::plus<f32>());
        f32 mean = sum / data_nsubsmp;
        
        std::ranges::transform(data, data.begin(), [mean](f32 val) { return val - mean; });

        std::vector<f32> corr_quality = correlate_valid(data, _corr_buffer);

        std::vector<f32> corr_kernel(kernel_size);
        auto zip_kernels = std::views::zip(slope_finder, _corr_buffer);
        std::ranges::transform(zip_kernels, corr_kernel.begin(), [this](const auto& pair) {
            return std::get<0>(pair) + (std::get<1>(pair) * buffer_strength);
        });

        std::vector<f32> corr = correlate_valid(data, corr_kernel);

        std::vector<f32> peaks(corr_nsamp);
        std::ranges::transform(corr_quality, peaks.begin(), [this](f32 val) { return val * buffer_strength; });

        if (edge_strength > 0.0f) {
            f32 cumsum = 0.0f;
            int start_edge = A - 1;
            int end_edge = data_nsubsmp - B;

            for (int i : std::views::iota(0, corr_nsamp)) {
                if ((start_edge + i) < end_edge) {
                    cumsum += data[start_edge + i];
                }
                peaks[i] += (cumsum * (-edge_strength));
            }
        }

        int peak_offset = find_peak(corr, peaks, trigger_radius);
        int current_trigger = trigger_begin + (_stride * peak_offset);

        _prev_trigger = std::max(current_trigger, _prev_trigger);

        return TriggerResult{_prev_trigger, std::nullopt};
    }

    void _update_buffer(std::vector<f32>& data, const PerFrameCache& cache) {
        assert(cache.result_mean.has_value());
        assert(cache.period.has_value());

        if (buffer_strength > 0.0f && responsiveness > 0.0f) {
            size_t n = data.size();
            if (n != _corr_buffer.size()) {
                throw std::invalid_argument("La taille des donnees ne correspond pas a celle du buffer de correlation.");
            }

            f32* data_ptr = data.data();
            f32 mean = cache.result_mean.value();

            #pragma omp simd
            for (size_t i = 0; i < n; ++i) {
                data_ptr[i] -= mean;
            }

            normalize_buffer_simd(data);

            f32 subsampled_period = cache.period.value() / static_cast<f32>(_stride);
            _prev_window = generate_gaussian_window(n, calc_buffer_std(subsampled_period));

            const f32* win_ptr = _prev_window.data();
            #pragma omp simd
            for (size_t i = 0; i < n; ++i) {
                data_ptr[i] *= win_ptr[i];
            }

            normalize_buffer_simd(_corr_buffer);
            _corr_buffer = lerp_simd(_corr_buffer, data, responsiveness);
        }
    }
};
