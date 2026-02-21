#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "samplerate.h"

struct SRC_STATE {
    src_callback_t cb;
    void *cb_data;
    int channels;
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
    return state;
}

long src_callback_read(SRC_STATE *state, double ratio, long frames, float *data)
{
    (void)ratio;
    if (!state || !state->cb || !data || frames <= 0) {
        return 0;
    }

    float *in = NULL;
    long got = state->cb(state->cb_data, &in);
    if (got <= 0 || in == NULL) {
        return 0;
    }

    long to_copy = frames < got ? frames : got;
    memcpy(data, in, sizeof(float) * to_copy * state->channels);
    return to_copy;
}

int src_reset(SRC_STATE *state)
{
    (void)state;
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
