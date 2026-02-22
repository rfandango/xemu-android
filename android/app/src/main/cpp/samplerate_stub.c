#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "samplerate.h"

struct SRC_STATE {
    src_callback_t cb;
    void *cb_data;
    int channels;

    float *input_buf;
    int input_buf_len;
    int input_buf_used;
    double input_pos;
};

SRC_STATE *src_callback_new(src_callback_t cb, int converter_type, int channels,
                            int *error, void *cb_data)
{
    (void)converter_type;
    if (error) {
        *error = 0;
    }
    SRC_STATE *state = (SRC_STATE *)calloc(1, sizeof(*state));
    if (!state) {
        if (error) {
            *error = -1;
        }
        return NULL;
    }
    state->cb = cb;
    state->cb_data = cb_data;
    state->channels = channels;
    state->input_buf = NULL;
    state->input_buf_len = 0;
    state->input_buf_used = 0;
    state->input_pos = 0.0;
    return state;
}

static int src_refill_input(SRC_STATE *state)
{
    int ch = state->channels;
    int consumed = (int)state->input_pos;
    state->input_pos -= consumed;
    if (state->input_pos < 0.0) {
        state->input_pos = 0.0;
    }

    int remaining = state->input_buf_len - state->input_buf_used - consumed;
    if (remaining < 0) {
        remaining = 0;
    }

    float *new_data = NULL;
    long got = state->cb(state->cb_data, &new_data);
    if (got <= 0 || new_data == NULL) {
        if (remaining > 0 && state->input_buf) {
            memmove(state->input_buf,
                    state->input_buf + (state->input_buf_used + consumed) * ch,
                    sizeof(float) * remaining * ch);
            state->input_buf_len = remaining;
            state->input_buf_used = 0;
        }
        return remaining >= 2;
    }

    int new_len = remaining + (int)got;
    float *buf = (float *)malloc(sizeof(float) * new_len * ch);
    if (!buf) {
        return 0;
    }

    if (remaining > 0 && state->input_buf) {
        memcpy(buf,
               state->input_buf + (state->input_buf_used + consumed) * ch,
               sizeof(float) * remaining * ch);
    }
    memcpy(buf + remaining * ch, new_data, sizeof(float) * got * ch);

    free(state->input_buf);
    state->input_buf = buf;
    state->input_buf_len = new_len;
    state->input_buf_used = 0;

    return 1;
}

long src_callback_read(SRC_STATE *state, double ratio, long frames, float *data)
{
    if (!state || !state->cb || !data || frames <= 0) {
        return 0;
    }

    if (ratio <= 0.0) {
        ratio = 1.0;
    }

    int ch = state->channels;
    double step = 1.0 / ratio;
    long out_frames = 0;

    while (out_frames < frames) {
        int avail = state->input_buf_len - state->input_buf_used;
        int needed_idx = (int)state->input_pos + 1;

        if (needed_idx >= avail) {
            if (!src_refill_input(state)) {
                break;
            }
            avail = state->input_buf_len - state->input_buf_used;
            if (avail < 2) {
                break;
            }
        }

        int idx0 = (int)state->input_pos;
        int idx1 = idx0 + 1;
        if (idx1 >= avail) {
            break;
        }

        float frac = (float)(state->input_pos - idx0);
        float *s0 = state->input_buf + (state->input_buf_used + idx0) * ch;
        float *s1 = state->input_buf + (state->input_buf_used + idx1) * ch;

        for (int c = 0; c < ch; c++) {
            data[out_frames * ch + c] = s0[c] + frac * (s1[c] - s0[c]);
        }

        out_frames++;
        state->input_pos += step;
    }

    return out_frames;
}

int src_reset(SRC_STATE *state)
{
    if (state) {
        free(state->input_buf);
        state->input_buf = NULL;
        state->input_buf_len = 0;
        state->input_buf_used = 0;
        state->input_pos = 0.0;
    }
    return 0;
}

const char *src_strerror(int error)
{
    (void)error;
    return "libsamplerate stub";
}

void src_float_to_short_array(const float *in, short *out, int len)
{
    if (!in || !out || len <= 0) {
        return;
    }

#if defined(__aarch64__)
    float32x4_t scale = vdupq_n_f32(32767.0f);
    float32x4_t hi = vdupq_n_f32(1.0f);
    float32x4_t lo = vdupq_n_f32(-1.0f);
    int i = 0;
    for (; i + 4 <= len; i += 4) {
        float32x4_t v = vld1q_f32(&in[i]);
        v = vminq_f32(vmaxq_f32(v, lo), hi);
        v = vmulq_f32(v, scale);
        int32x4_t iv = vcvtq_s32_f32(v);
        int16x4_t sv = vqmovn_s32(iv);
        vst1_s16(&out[i], sv);
    }
    for (; i < len; i++) {
        float v = in[i];
        if (v > 1.0f) v = 1.0f;
        else if (v < -1.0f) v = -1.0f;
        out[i] = (short)(v * 32767.0f);
    }
#else
    for (int i = 0; i < len; ++i) {
        float v = in[i];
        if (v > 1.0f) {
            v = 1.0f;
        } else if (v < -1.0f) {
            v = -1.0f;
        }
        out[i] = (short)(v * 32767.0f);
    }
#endif
}
