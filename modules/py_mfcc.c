/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2026 Sarama
 *
 * Helium/CMSIS-backed MFCC and bark-analysis helpers for Sarama firmware.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <arm_math.h>

#include "py/obj.h"
#include "py/objarray.h"
#include "py/objstr.h"
#include "py/runtime.h"

#ifndef M_PI
#define M_PI (3.14159265358979323846)
#endif

#define MFCC_DEFAULT_SR             (16000)
#define MFCC_COEFFS                 (20)
#define MFCC_MEL_BINS               (128)
#define MFCC_FEATURES_LEN           (MFCC_COEFFS * 4)
#define MFCC_BARK_FEATURES_LEN      (14)
#define MFCC_WINDOW_MS              (50)
#define MFCC_HOP_MS                 (10)
#define MFCC_MAX_FFT_LEN            (4096)
#define MFCC_EPSILON                (1.0e-10f)
#define MFCC_PREEMPH                (0.97f)

#define BARK_LOW_HZ                 (300.0f)
#define BARK_MID_HZ                 (2000.0f)
#define BARK_HIGH_HZ                (8000.0f)

typedef struct _mfcc_fft_plan_t {
    uint32_t sr;
    uint32_t sample_count;
    uint32_t frame_len;
    uint32_t hop_len;
    uint32_t fft_len;
    uint32_t spectrum_bins;
    uint32_t frame_count;
    arm_rfft_fast_instance_f32 rfft;
    float *window;
    float *fft_in;
    float *fft_out;
    float *power;
    uint16_t mel_bins[MFCC_MEL_BINS + 2];
} mfcc_fft_plan_t;

static bool mfcc_dct_ready = false;
static float mfcc_dct[MFCC_COEFFS][MFCC_MEL_BINS];

static inline float mfcc_hz_to_mel(float hz) {
    return 2595.0f * log10f(1.0f + (hz / 700.0f));
}

static inline float mfcc_mel_to_hz(float mel) {
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

static inline uint32_t mfcc_next_pow2(uint32_t value) {
    uint32_t out = 1;
    while (out < value) {
        out <<= 1;
    }
    return out;
}

static inline int16_t mfcc_read_s16le(const uint8_t *src) {
    return (int16_t) ((src[0] & 0xFF) | (src[1] << 8));
}

static inline int16_t mfcc_clip_s16(float value) {
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (int16_t) value;
}

static void mfcc_build_dct(void) {
    if (mfcc_dct_ready) {
        return;
    }

    const float inv_n = 1.0f / (float) MFCC_MEL_BINS;
    const float dc_scale = sqrtf(inv_n);
    const float ac_scale = sqrtf(2.0f * inv_n);

    for (uint32_t k = 0; k < MFCC_COEFFS; k++) {
        const float scale = (k == 0) ? dc_scale : ac_scale;
        for (uint32_t n = 0; n < MFCC_MEL_BINS; n++) {
            mfcc_dct[k][n] = scale * cosf(((float) M_PI / (float) MFCC_MEL_BINS) * ((float) n + 0.5f) * (float) k);
        }
    }

    mfcc_dct_ready = true;
}

static void mfcc_fill_window(float *window, uint32_t frame_len) {
    if (frame_len <= 1) {
        window[0] = 1.0f;
        return;
    }

    const float denom = (float) (frame_len - 1);
    for (uint32_t i = 0; i < frame_len; i++) {
        window[i] = 0.5f - (0.5f * cosf((2.0f * (float) M_PI * (float) i) / denom));
    }
}

static void mfcc_fill_mel_bins(mfcc_fft_plan_t *plan, float fmax_hz) {
    const float nyquist = (float) plan->sr * 0.5f;
    if (fmax_hz > nyquist) {
        fmax_hz = nyquist;
    }
    if (fmax_hz < 100.0f) {
        fmax_hz = nyquist;
    }

    const float mel_min = mfcc_hz_to_mel(0.0f);
    const float mel_max = mfcc_hz_to_mel(fmax_hz);
    const float mel_step = (mel_max - mel_min) / (float) (MFCC_MEL_BINS + 1);

    for (uint32_t i = 0; i < MFCC_MEL_BINS + 2; i++) {
        const float mel = mel_min + ((float) i * mel_step);
        const float hz = mfcc_mel_to_hz(mel);
        int32_t bin = (int32_t) (((float) (plan->fft_len + 1) * hz) / (float) plan->sr);
        if (bin < 0) {
            bin = 0;
        }
        if (bin > (int32_t) plan->spectrum_bins - 1) {
            bin = (int32_t) plan->spectrum_bins - 1;
        }
        plan->mel_bins[i] = (uint16_t) bin;
    }

    for (uint32_t i = 1; i < MFCC_MEL_BINS + 2; i++) {
        uint16_t min_bin = plan->mel_bins[i - 1];
        if (plan->mel_bins[i] <= min_bin) {
            uint16_t next_bin = min_bin + 1;
            if (next_bin >= plan->spectrum_bins) {
                next_bin = plan->spectrum_bins - 1;
            }
            plan->mel_bins[i] = next_bin;
        }
    }
}

static bool mfcc_init_fft_plan(mfcc_fft_plan_t *plan, uint32_t sample_count, uint32_t sr, float fmax_hz) {
    memset(plan, 0, sizeof(*plan));

    plan->sr = sr ? sr : MFCC_DEFAULT_SR;
    plan->sample_count = sample_count;
    plan->frame_len = (plan->sr * MFCC_WINDOW_MS) / 1000;
    plan->hop_len = (plan->sr * MFCC_HOP_MS) / 1000;

    if (plan->frame_len < 32) {
        plan->frame_len = 32;
    }
    if (plan->hop_len < 1) {
        plan->hop_len = 1;
    }

    plan->fft_len = mfcc_next_pow2(plan->frame_len);
    if (plan->fft_len > MFCC_MAX_FFT_LEN) {
        return false;
    }

    plan->spectrum_bins = (plan->fft_len / 2) + 1;
    if (sample_count <= plan->frame_len) {
        plan->frame_count = 1;
    } else {
        plan->frame_count = 1 + ((sample_count - plan->frame_len) / plan->hop_len);
    }

    if (arm_rfft_fast_init_f32(&plan->rfft, plan->fft_len) != ARM_MATH_SUCCESS) {
        return false;
    }

    plan->window = m_new(float, plan->frame_len);
    plan->fft_in = m_new(float, plan->fft_len);
    plan->fft_out = m_new(float, plan->fft_len);
    plan->power = m_new(float, plan->spectrum_bins);

    mfcc_fill_window(plan->window, plan->frame_len);
    mfcc_fill_mel_bins(plan, fmax_hz);
    return true;
}

static void mfcc_free_fft_plan(mfcc_fft_plan_t *plan) {
    if (plan->window != NULL) {
        m_del(float, plan->window, plan->frame_len);
    }
    if (plan->fft_in != NULL) {
        m_del(float, plan->fft_in, plan->fft_len);
    }
    if (plan->fft_out != NULL) {
        m_del(float, plan->fft_out, plan->fft_len);
    }
    if (plan->power != NULL) {
        m_del(float, plan->power, plan->spectrum_bins);
    }
}

static void mfcc_load_frame(const float *samples, const mfcc_fft_plan_t *plan, uint32_t frame_index, bool preemphasis) {
    const uint32_t offset = frame_index * plan->hop_len;
    memset(plan->fft_in, 0, sizeof(float) * plan->fft_len);

    for (uint32_t i = 0; i < plan->frame_len; i++) {
        uint32_t src_index = offset + i;
        float current = (src_index < plan->sample_count) ? samples[src_index] : 0.0f;
        if (preemphasis) {
            float previous = 0.0f;
            if (src_index > 0 && (src_index - 1) < plan->sample_count) {
                previous = samples[src_index - 1];
            }
            current -= (MFCC_PREEMPH * previous);
        }
        plan->fft_in[i] = current;
    }

    arm_mult_f32(plan->fft_in, plan->window, plan->fft_in, plan->frame_len);
}

static void mfcc_compute_power_spectrum(mfcc_fft_plan_t *plan) {
    arm_rfft_fast_f32(&plan->rfft, plan->fft_in, plan->fft_out, 0);

    plan->power[0] = plan->fft_out[0] * plan->fft_out[0];
    for (uint32_t k = 1; k < (plan->fft_len / 2); k++) {
        float re = plan->fft_out[2 * k];
        float im = plan->fft_out[(2 * k) + 1];
        plan->power[k] = (re * re) + (im * im);
    }
    plan->power[plan->fft_len / 2] = plan->fft_out[1] * plan->fft_out[1];
}

static void mfcc_compute_log_mel(const mfcc_fft_plan_t *plan, float *mel_out) {
    for (uint32_t m = 0; m < MFCC_MEL_BINS; m++) {
        uint32_t start = plan->mel_bins[m];
        uint32_t center = plan->mel_bins[m + 1];
        uint32_t end = plan->mel_bins[m + 2];
        float energy = 0.0f;

        if (center <= start) {
            center = start + 1;
        }
        if (end <= center) {
            end = center + 1;
        }
        if (end >= plan->spectrum_bins) {
            end = plan->spectrum_bins - 1;
        }

        float denom_up = (float) (center - start);
        float denom_down = (float) (end - center);

        for (uint32_t k = start; k < center && k < plan->spectrum_bins; k++) {
            float weight = ((float) (k - start)) / denom_up;
            energy += plan->power[k] * weight;
        }
        for (uint32_t k = center; k <= end && k < plan->spectrum_bins; k++) {
            float weight = ((float) (end - k)) / denom_down;
            if (weight < 0.0f) {
                weight = 0.0f;
            }
            energy += plan->power[k] * weight;
        }

        mel_out[m] = 10.0f * log10f(energy + MFCC_EPSILON);
    }
}

static void mfcc_compute_dct(const float *mel_in, float *mfcc_out) {
    for (uint32_t k = 0; k < MFCC_COEFFS; k++) {
        float value = 0.0f;
        arm_dot_prod_f32(mel_in, mfcc_dct[k], MFCC_MEL_BINS, &value);
        mfcc_out[k] = value;
    }
}

static void mfcc_compute_band_ratios(const mfcc_fft_plan_t *plan, float *r_low, float *r_bark, float *r_high) {
    const uint32_t low_end = (uint32_t) (((BARK_LOW_HZ * (float) plan->fft_len) / (float) plan->sr));
    const uint32_t bark_end = (uint32_t) (((BARK_MID_HZ * (float) plan->fft_len) / (float) plan->sr));
    const float high_cap = ((float) plan->sr * 0.5f < BARK_HIGH_HZ) ? ((float) plan->sr * 0.5f) : BARK_HIGH_HZ;
    const uint32_t high_end = (uint32_t) (((high_cap * (float) plan->fft_len) / (float) plan->sr));

    float low = 0.0f;
    float bark = 0.0f;
    float high = 0.0f;

    for (uint32_t i = 0; i < plan->spectrum_bins; i++) {
        float value = plan->power[i];
        if (i < low_end) {
            low += value;
        } else if (i < bark_end) {
            bark += value;
        } else if (i <= high_end) {
            high += value;
        }
    }

    float total = low + bark + high + MFCC_EPSILON;
    *r_low = low / total;
    *r_bark = bark / total;
    *r_high = high / total;
}

static mp_obj_t mfcc_raise_bad_pcm(void) {
    mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("PCM buffer must contain little-endian int16 mono samples"));
    return mp_const_none;
}

static mp_obj_t py_mfcc_extract(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_pcm, ARG_sr };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pcm, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_sr, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = MFCC_DEFAULT_SR} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_pcm].u_obj, &bufinfo, MP_BUFFER_READ);
    if ((bufinfo.len < 2) || (bufinfo.len & 1)) {
        return mfcc_raise_bad_pcm();
    }

    uint32_t sample_count = bufinfo.len / 2;
    uint32_t sr = args[ARG_sr].u_int;
    mfcc_fft_plan_t plan;
    float *samples = NULL;
    float *mel = NULL;
    float *coeffs = NULL;
    mp_obj_t result = mp_const_none;

    if (!mfcc_init_fft_plan(&plan, sample_count, sr, BARK_HIGH_HZ)) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Unsupported MFCC sample rate or FFT size"));
    }

    mfcc_build_dct();

    samples = m_new(float, sample_count);
    mel = m_new(float, MFCC_MEL_BINS);
    coeffs = m_new(float, MFCC_COEFFS);

    const uint8_t *src = (const uint8_t *) bufinfo.buf;
    for (uint32_t i = 0; i < sample_count; i++) {
        samples[i] = (float) mfcc_read_s16le(src + (i * 2)) / 32768.0f;
    }

    float sums[MFCC_COEFFS];
    float sumsqs[MFCC_COEFFS];
    float mins[MFCC_COEFFS];
    float maxs[MFCC_COEFFS];

    for (uint32_t i = 0; i < MFCC_COEFFS; i++) {
        sums[i] = 0.0f;
        sumsqs[i] = 0.0f;
        mins[i] = INFINITY;
        maxs[i] = -INFINITY;
    }

    for (uint32_t frame = 0; frame < plan.frame_count; frame++) {
        mfcc_load_frame(samples, &plan, frame, false);
        mfcc_compute_power_spectrum(&plan);
        mfcc_compute_log_mel(&plan, mel);
        mfcc_compute_dct(mel, coeffs);

        for (uint32_t i = 0; i < MFCC_COEFFS; i++) {
            float value = coeffs[i];
            sums[i] += value;
            sumsqs[i] += value * value;
            if (value < mins[i]) {
                mins[i] = value;
            }
            if (value > maxs[i]) {
                maxs[i] = value;
            }
        }
    }

    mp_obj_t *items = m_new(mp_obj_t, MFCC_FEATURES_LEN);
    const float inv_frames = 1.0f / (float) plan.frame_count;

    for (uint32_t i = 0; i < MFCC_COEFFS; i++) {
        float mean = sums[i] * inv_frames;
        float variance = (sumsqs[i] * inv_frames) - (mean * mean);
        if (variance < 0.0f) {
            variance = 0.0f;
        }
        items[i] = mp_obj_new_float(mean);
        items[MFCC_COEFFS + i] = mp_obj_new_float(sqrtf(variance));
        items[(2 * MFCC_COEFFS) + i] = mp_obj_new_float(maxs[i]);
        items[(3 * MFCC_COEFFS) + i] = mp_obj_new_float(mins[i]);
    }

    result = mp_obj_new_tuple(MFCC_FEATURES_LEN, items);
    m_del(mp_obj_t, items, MFCC_FEATURES_LEN);
    m_del(float, coeffs, MFCC_COEFFS);
    m_del(float, mel, MFCC_MEL_BINS);
    m_del(float, samples, sample_count);
    mfcc_free_fft_plan(&plan);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(py_mfcc_extract_obj, 1, py_mfcc_extract);

static mp_obj_t py_mfcc_bark_energies(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_pcm, ARG_sr };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pcm, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_sr, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = MFCC_DEFAULT_SR} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_pcm].u_obj, &bufinfo, MP_BUFFER_READ);
    if ((bufinfo.len < 2) || (bufinfo.len & 1)) {
        return mfcc_raise_bad_pcm();
    }

    uint32_t sample_count = bufinfo.len / 2;
    uint32_t sr = args[ARG_sr].u_int;
    mfcc_fft_plan_t plan;
    float *samples = NULL;
    float acc_low = 0.0f;
    float acc_bark = 0.0f;
    float acc_high = 0.0f;

    if (!mfcc_init_fft_plan(&plan, sample_count, sr, BARK_HIGH_HZ)) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("Unsupported MFCC sample rate or FFT size"));
    }

    samples = m_new(float, sample_count);
    const uint8_t *src = (const uint8_t *) bufinfo.buf;
    for (uint32_t i = 0; i < sample_count; i++) {
        samples[i] = (float) mfcc_read_s16le(src + (i * 2)) / 32768.0f;
    }

    for (uint32_t frame = 0; frame < plan.frame_count; frame++) {
        float r_low;
        float r_bark;
        float r_high;
        mfcc_load_frame(samples, &plan, frame, true);
        mfcc_compute_power_spectrum(&plan);
        mfcc_compute_band_ratios(&plan, &r_low, &r_bark, &r_high);
        acc_low += r_low;
        acc_bark += r_bark;
        acc_high += r_high;
    }

    const float inv = 1.0f / (float) plan.frame_count;
    mp_obj_t tuple = mp_obj_new_tuple(3, (mp_obj_t []) {
        mp_obj_new_float(acc_low * inv),
        mp_obj_new_float(acc_bark * inv),
        mp_obj_new_float(acc_high * inv),
    });

    m_del(float, samples, sample_count);
    mfcc_free_fft_plan(&plan);
    return tuple;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(py_mfcc_bark_energies_obj, 1, py_mfcc_bark_energies);

static mp_obj_t py_mfcc_bark_features(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_pcm, ARG_sr };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pcm, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_sr, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = MFCC_DEFAULT_SR} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_pcm].u_obj, &bufinfo, MP_BUFFER_READ);
    if ((bufinfo.len < 2) || (bufinfo.len & 1)) {
        return mfcc_raise_bad_pcm();
    }

    uint32_t sample_count = bufinfo.len / 2;
    const uint8_t *src = (const uint8_t *) bufinfo.buf;
    float peak = 0.0f;
    float abs_sum = 0.0f;
    float sq_sum = 0.0f;
    float diff_sum = 0.0f;
    uint32_t zero_crossings = 0;
    int16_t prev = 0;
    int prev_sign = 0;

    for (uint32_t i = 0; i < sample_count; i++) {
        int16_t sample = mfcc_read_s16le(src + (i * 2));
        float abs_sample = (sample < 0) ? -(float) sample : (float) sample;
        abs_sum += abs_sample;
        sq_sum += (float) sample * (float) sample;
        if (abs_sample > peak) {
            peak = abs_sample;
        }

        int sign = (sample >= 0) ? 1 : -1;
        if (i > 0) {
            diff_sum += fabsf((float) sample - (float) prev);
            if (sign != prev_sign) {
                zero_crossings++;
            }
        }

        prev = sample;
        prev_sign = sign;
    }

    mp_obj_t bark_tuple = py_mfcc_bark_energies(1, (mp_obj_t []) {args[ARG_pcm].u_obj}, kw_args);
    mp_obj_tuple_t *bark_items = MP_OBJ_TO_PTR(bark_tuple);
    float r_low = mp_obj_get_float_to_f(bark_items->items[0]);
    float r_bark = mp_obj_get_float_to_f(bark_items->items[1]);
    float r_high = mp_obj_get_float_to_f(bark_items->items[2]);

    float mean_abs = abs_sum / (float) sample_count;
    float rms = sqrtf(sq_sum / (float) sample_count);
    float crest = (rms > 1.0f) ? (peak / rms) : 0.0f;
    float zcr = (sample_count > 1) ? ((float) zero_crossings / (float) (sample_count - 1)) : 0.0f;
    float flatness = (rms > 0.0f) ? (mean_abs / rms) : 1.0f;
    float flux_mean = (sample_count > 1) ? ((diff_sum / (float) (sample_count - 1)) / 65535.0f) : 0.0f;

    return mp_obj_new_tuple(MFCC_BARK_FEATURES_LEN, (mp_obj_t []) {
        mp_obj_new_float(rms / 32768.0f),
        mp_obj_new_float(peak / 32768.0f),
        mp_obj_new_float(crest),
        mp_obj_new_float(zcr),
        mp_obj_new_float(r_low),
        mp_obj_new_float(r_bark),
        mp_obj_new_float(r_high),
        mp_obj_new_float(r_bark - r_low),
        mp_obj_new_float(0.0f),
        mp_obj_new_float(0.0f),
        mp_obj_new_float(0.0f),
        mp_obj_new_float(flatness),
        mp_obj_new_float(0.0f),
        mp_obj_new_float(flux_mean),
    });
}
static MP_DEFINE_CONST_FUN_OBJ_KW(py_mfcc_bark_features_obj, 1, py_mfcc_bark_features);

static mp_obj_t py_mfcc_clean_audio(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_pcm, ARG_sr, ARG_click_thresh, ARG_gate_floor };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pcm, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_sr, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = MFCC_DEFAULT_SR} },
        { MP_QSTR_click_thresh, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_gate_floor, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_pcm].u_obj, &bufinfo, MP_BUFFER_READ);
    if ((bufinfo.len < 2) || (bufinfo.len & 1)) {
        return mfcc_raise_bad_pcm();
    }

    (void) args[ARG_sr].u_int;

    float click_thresh = (args[ARG_click_thresh].u_obj == mp_const_none) ? 6.0f : mp_obj_get_float_to_f(args[ARG_click_thresh].u_obj);
    float gate_floor = (args[ARG_gate_floor].u_obj == mp_const_none) ? 0.02f : mp_obj_get_float_to_f(args[ARG_gate_floor].u_obj);

    uint32_t sample_count = bufinfo.len / 2;
    float *samples = m_new(float, sample_count);
    byte *out = m_new(byte, bufinfo.len);
    const uint8_t *src = (const uint8_t *) bufinfo.buf;

    float peak = 0.0f;
    float sq_sum = 0.0f;

    for (uint32_t i = 0; i < sample_count; i++) {
        float value = (float) mfcc_read_s16le(src + (i * 2)) / 32768.0f;
        samples[i] = value;
        float abs_value = fabsf(value);
        if (abs_value > peak) {
            peak = abs_value;
        }
        sq_sum += value * value;
    }

    float rms = sqrtf(sq_sum / (float) sample_count);
    float click_limit = click_thresh * rms;
    float gate_limit = gate_floor * peak;

    for (uint32_t i = 1; i + 1 < sample_count; i++) {
        float current = samples[i];
        float local_mean = 0.5f * (samples[i - 1] + samples[i + 1]);
        float deviation = fabsf(current - local_mean);
        if (deviation > click_limit &&
            fabsf(current - samples[i - 1]) > click_limit &&
            fabsf(current - samples[i + 1]) > click_limit) {
            samples[i] = local_mean;
        }
    }

    float prev_in = 0.0f;
    float prev_out = 0.0f;
    for (uint32_t i = 0; i < sample_count; i++) {
        float value = samples[i];
        if (fabsf(value) < gate_limit) {
            value = 0.0f;
        }
        float hp = value - prev_in + (0.995f * prev_out);
        prev_in = value;
        prev_out = hp;
        int16_t s16 = mfcc_clip_s16(hp * 32767.0f);
        out[i * 2] = s16 & 0xFF;
        out[(i * 2) + 1] = (s16 >> 8) & 0xFF;
    }

    mp_obj_t result = mp_obj_new_bytes(out, bufinfo.len);
    m_del(byte, out, bufinfo.len);
    m_del(float, samples, sample_count);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(py_mfcc_clean_audio_obj, 1, py_mfcc_clean_audio);

static const mp_rom_map_elem_t mfcc_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mfcc) },
    { MP_ROM_QSTR(MP_QSTR_extract), MP_ROM_PTR(&py_mfcc_extract_obj) },
    { MP_ROM_QSTR(MP_QSTR_bark_energies), MP_ROM_PTR(&py_mfcc_bark_energies_obj) },
    { MP_ROM_QSTR(MP_QSTR_bark_features), MP_ROM_PTR(&py_mfcc_bark_features_obj) },
    { MP_ROM_QSTR(MP_QSTR_clean_audio), MP_ROM_PTR(&py_mfcc_clean_audio_obj) },
};

static MP_DEFINE_CONST_DICT(mfcc_module_globals, mfcc_module_globals_table);

const mp_obj_module_t mfcc_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *) &mfcc_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_mfcc, mfcc_user_cmodule);
