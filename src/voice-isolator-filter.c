#include <obs-module.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <media-io/audio-resampler.h>
#include <util/bmem.h>
#include <util/deque.h>
#include <util/threading.h>

#ifdef _MSC_VER
#define ssize_t intptr_t
#endif
#include <rnnoise.h>

#define VOICE_ISOLATOR_MAX_CHANNELS 8
#define RNNOISE_SAMPLE_RATE 48000
#define RNNOISE_FRAME_SIZE 480
#define SEGMENT_MS 10
#define REFERENCE_BUFFER_SECONDS 2
#define NLMS_TAPS 128
#define NS_PER_SECOND 1000000000ULL

#define S_VOICE_THRESHOLD "voice_threshold"
#define S_BREATH_SUPPRESSION "breath_suppression"
#define S_GATE_RELEASE_MS "gate_release_ms"
#define S_NOISE_FLOOR_DB "noise_floor_db"
#define S_AUX_SOURCE "aux_source"
#define S_AUX_CANCELLATION "aux_cancellation"
#define S_AUX_ADAPT_RATE "aux_adapt_rate"
#define S_AUX_OFFSET_MS "aux_offset_ms"

struct packet_info {
    uint32_t frames;
    uint64_t timestamp;
};

struct voice_isolator_data {
    obs_source_t *context;

    uint32_t sample_rate;
    size_t channels;
    size_t segment_frames;
    uint64_t latency_ns;
    uint64_t last_timestamp;

    struct deque info_buffer;
    struct deque input_buffers[VOICE_ISOLATOR_MAX_CHANNELS];
    struct deque output_buffers[VOICE_ISOLATOR_MAX_CHANNELS];
    struct deque aux_input_buffer;

    float *copy_storage;
    float *copy_buffers[VOICE_ISOLATOR_MAX_CHANNELS];
    float *rnnoise_storage;
    float *rnnoise_buffers[VOICE_ISOLATOR_MAX_CHANNELS];
    float *segment_aux;

    DenoiseState *rnnoise_states[VOICE_ISOLATOR_MAX_CHANNELS];
    audio_resampler_t *to_rnnoise;
    audio_resampler_t *from_rnnoise;

    struct obs_audio_data output_audio;
    float *output_storage;
    size_t output_capacity_floats;

    float voice_threshold;
    float breath_suppression;
    float noise_floor_db;
    float aux_cancellation;
    float aux_adapt_rate;
    int gate_release_ms;
    int aux_offset_ms;

    float gate_gain;
    float last_vad;
    int hangover_segments;

    float *nlms_weights;
    float reference_history[NLMS_TAPS];
    size_t reference_position;

    pthread_mutex_t aux_mutex;
    struct deque aux_samples;
    uint64_t aux_start_timestamp;
    uint64_t aux_end_timestamp;
    bool aux_timeline_valid;

    obs_source_t *aux_source;
    char *aux_source_name;
    float *aux_callback_buffer;
    size_t aux_callback_capacity;
    float *aux_packet_buffer;
    size_t aux_packet_capacity;
};

static inline float clampf(float value, float minimum, float maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static inline uint64_t frames_to_ns(size_t frames, uint32_t sample_rate)
{
    if (sample_rate == 0)
        return 0;

    return (uint64_t)((double)frames * (double)NS_PER_SECOND / (double)sample_rate);
}

static inline size_t ns_to_frames(uint64_t nanoseconds, uint32_t sample_rate)
{
    return (size_t)((double)nanoseconds * (double)sample_rate / (double)NS_PER_SECOND);
}

static inline void clear_deque(struct deque *buffer)
{
    if (buffer->size > 0)
        deque_pop_front(buffer, NULL, buffer->size);
}

static enum speaker_layout speaker_layout_from_channels(size_t channels)
{
    switch (channels) {
    case 1:
        return SPEAKERS_MONO;
    case 2:
        return SPEAKERS_STEREO;
    case 3:
        return SPEAKERS_2POINT1;
    case 4:
        return SPEAKERS_4POINT0;
    case 5:
        return SPEAKERS_4POINT1;
    case 6:
        return SPEAKERS_5POINT1;
    case 8:
        return SPEAKERS_7POINT1;
    default:
        return SPEAKERS_UNKNOWN;
    }
}

static void reset_adaptive_filter(struct voice_isolator_data *filter)
{
    if (filter->nlms_weights)
        memset(filter->nlms_weights, 0, filter->channels * NLMS_TAPS * sizeof(float));

    memset(filter->reference_history, 0, sizeof(filter->reference_history));
    filter->reference_position = 0;
}

static void reset_processing_buffers(struct voice_isolator_data *filter)
{
    for (size_t channel = 0; channel < filter->channels; channel++) {
        clear_deque(&filter->input_buffers[channel]);
        clear_deque(&filter->output_buffers[channel]);
    }

    clear_deque(&filter->aux_input_buffer);
    clear_deque(&filter->info_buffer);
    reset_adaptive_filter(filter);
    filter->gate_gain = 0.0f;
    filter->last_vad = 0.0f;
    filter->hangover_segments = 0;
}

static void clear_aux_timeline_locked(struct voice_isolator_data *filter)
{
    clear_deque(&filter->aux_samples);
    filter->aux_start_timestamp = 0;
    filter->aux_end_timestamp = 0;
    filter->aux_timeline_valid = false;
}

static void auxiliary_audio_capture(void *parameter, obs_source_t *source,
                                    const struct audio_data *audio, bool muted)
{
    struct voice_isolator_data *filter = parameter;

    if (!filter || source != filter->aux_source || !audio || audio->frames == 0)
        return;

    pthread_mutex_lock(&filter->aux_mutex);

    if (muted || !audio->data[0]) {
        clear_aux_timeline_locked(filter);
        pthread_mutex_unlock(&filter->aux_mutex);
        return;
    }

    if (audio->frames > filter->aux_callback_capacity) {
        float *replacement =
            brealloc(filter->aux_callback_buffer, audio->frames * sizeof(float));

        if (!replacement) {
            pthread_mutex_unlock(&filter->aux_mutex);
            return;
        }

        filter->aux_callback_buffer = replacement;
        filter->aux_callback_capacity = audio->frames;
    }

    for (uint32_t frame = 0; frame < audio->frames; frame++) {
        float sum = 0.0f;
        size_t planes = 0;

        for (size_t channel = 0; channel < filter->channels; channel++) {
            if (!audio->data[channel])
                continue;

            const float *samples = (const float *)audio->data[channel];
            sum += samples[frame];
            planes++;
        }

        filter->aux_callback_buffer[frame] =
            planes > 0 ? sum / (float)planes : 0.0f;
    }

    const uint64_t packet_duration =
        frames_to_ns(audio->frames, filter->sample_rate);
    const uint64_t discontinuity_limit = 50ULL * 1000ULL * 1000ULL;

    if (!filter->aux_timeline_valid) {
        filter->aux_start_timestamp = audio->timestamp;
        filter->aux_end_timestamp = audio->timestamp;
        filter->aux_timeline_valid = true;
    } else {
        const int64_t delta =
            (int64_t)audio->timestamp - (int64_t)filter->aux_end_timestamp;

        if (llabs(delta) > (int64_t)discontinuity_limit) {
            clear_aux_timeline_locked(filter);
            filter->aux_start_timestamp = audio->timestamp;
            filter->aux_end_timestamp = audio->timestamp;
            filter->aux_timeline_valid = true;
        }
    }

    deque_push_back(&filter->aux_samples, filter->aux_callback_buffer,
                    audio->frames * sizeof(float));
    filter->aux_end_timestamp = audio->timestamp + packet_duration;

    const size_t maximum_frames =
        (size_t)filter->sample_rate * REFERENCE_BUFFER_SECONDS;
    const size_t buffered_frames =
        filter->aux_samples.size / sizeof(float);

    if (buffered_frames > maximum_frames) {
        const size_t frames_to_drop = buffered_frames - maximum_frames;
        deque_pop_front(&filter->aux_samples, NULL,
                        frames_to_drop * sizeof(float));
        filter->aux_start_timestamp +=
            frames_to_ns(frames_to_drop, filter->sample_rate);
    }

    pthread_mutex_unlock(&filter->aux_mutex);
}

static void detach_auxiliary_source(struct voice_isolator_data *filter)
{
    if (filter->aux_source) {
        obs_source_remove_audio_capture_callback(
            filter->aux_source, auxiliary_audio_capture, filter);
        obs_source_release(filter->aux_source);
        filter->aux_source = NULL;
    }

    pthread_mutex_lock(&filter->aux_mutex);
    clear_aux_timeline_locked(filter);
    pthread_mutex_unlock(&filter->aux_mutex);

    reset_adaptive_filter(filter);
}

static void attach_auxiliary_source(struct voice_isolator_data *filter,
                                    const char *source_name)
{
    if (!source_name)
        source_name = "";

    if (filter->aux_source_name &&
        strcmp(filter->aux_source_name, source_name) == 0)
        return;

    detach_auxiliary_source(filter);

    bfree(filter->aux_source_name);
    filter->aux_source_name = bstrdup(source_name);

    if (!*source_name)
        return;

    obs_source_t *source = obs_get_source_by_name(source_name);
    if (!source)
        return;

    obs_source_t *parent = obs_filter_get_parent(filter->context);
    const uint32_t flags = obs_source_get_output_flags(source);

    if (source == parent || !(flags & OBS_SOURCE_AUDIO)) {
        obs_source_release(source);
        return;
    }

    filter->aux_source = source;
    obs_source_add_audio_capture_callback(
        filter->aux_source, auxiliary_audio_capture, filter);
}

static bool read_aligned_auxiliary(struct voice_isolator_data *filter,
                                   uint64_t primary_timestamp, size_t frames,
                                   float *output)
{
    memset(output, 0, frames * sizeof(float));

    if (!filter->aux_source || frames == 0)
        return false;

    int64_t target_signed =
        (int64_t)primary_timestamp +
        (int64_t)filter->aux_offset_ms * 1000LL * 1000LL;

    if (target_signed < 0)
        target_signed = 0;

    const uint64_t target_timestamp = (uint64_t)target_signed;
    bool copied = false;

    pthread_mutex_lock(&filter->aux_mutex);

    if (!filter->aux_timeline_valid || filter->aux_samples.size == 0) {
        pthread_mutex_unlock(&filter->aux_mutex);
        return false;
    }

    if (target_timestamp >= filter->aux_end_timestamp) {
        pthread_mutex_unlock(&filter->aux_mutex);
        return false;
    }

    size_t leading_zeros = 0;

    if (target_timestamp > filter->aux_start_timestamp) {
        const size_t skip_frames =
            ns_to_frames(target_timestamp - filter->aux_start_timestamp,
                         filter->sample_rate);
        const size_t available_frames =
            filter->aux_samples.size / sizeof(float);
        const size_t actual_skip =
            skip_frames < available_frames ? skip_frames : available_frames;

        if (actual_skip > 0) {
            deque_pop_front(&filter->aux_samples, NULL,
                            actual_skip * sizeof(float));
            filter->aux_start_timestamp +=
                frames_to_ns(actual_skip, filter->sample_rate);
        }
    } else if (target_timestamp < filter->aux_start_timestamp) {
        leading_zeros =
            ns_to_frames(filter->aux_start_timestamp - target_timestamp,
                         filter->sample_rate);
        if (leading_zeros >= frames) {
            pthread_mutex_unlock(&filter->aux_mutex);
            return false;
        }
    }

    const size_t available_frames =
        filter->aux_samples.size / sizeof(float);
    const size_t requested_frames = frames - leading_zeros;
    const size_t copy_frames =
        requested_frames < available_frames ? requested_frames
                                             : available_frames;

    if (copy_frames > 0) {
        deque_peek_front(&filter->aux_samples, output + leading_zeros,
                         copy_frames * sizeof(float));
        copied = true;
    }

    pthread_mutex_unlock(&filter->aux_mutex);
    return copied;
}

static void adaptive_reference_cancel(struct voice_isolator_data *filter)
{
    if (!filter->aux_source || filter->aux_cancellation <= 0.001f)
        return;

    double reference_energy = 0.0;

    for (size_t frame = 0; frame < filter->segment_frames; frame++) {
        const float reference = filter->segment_aux[frame];
        reference_energy += (double)reference * (double)reference;
    }

    const float reference_rms =
        (float)sqrt(reference_energy /
                    (double)(filter->segment_frames > 0
                                 ? filter->segment_frames
                                 : 1));

    if (reference_rms < 0.0003f)
        return;

    const bool can_adapt = filter->last_vad < 0.25f;

    for (size_t frame = 0; frame < filter->segment_frames; frame++) {
        filter->reference_history[filter->reference_position] =
            filter->segment_aux[frame];

        for (size_t channel = 0; channel < filter->channels; channel++) {
            float estimate = 0.0f;
            float normalization = 1.0e-6f;
            float *weights =
                filter->nlms_weights + channel * NLMS_TAPS;

            size_t history_index = filter->reference_position;

            for (size_t tap = 0; tap < NLMS_TAPS; tap++) {
                const float reference =
                    filter->reference_history[history_index];
                estimate += weights[tap] * reference;
                normalization += reference * reference;

                history_index =
                    history_index == 0 ? NLMS_TAPS - 1
                                       : history_index - 1;
            }

            const float input = filter->copy_buffers[channel][frame];
            const float full_error = input - estimate;
            filter->copy_buffers[channel][frame] =
                input - estimate * filter->aux_cancellation;

            if (can_adapt) {
                const float step =
                    filter->aux_adapt_rate * full_error / normalization;
                history_index = filter->reference_position;

                for (size_t tap = 0; tap < NLMS_TAPS; tap++) {
                    const float reference =
                        filter->reference_history[history_index];
                    weights[tap] =
                        clampf(weights[tap] + step * reference, -2.0f,
                               2.0f);

                    history_index =
                        history_index == 0 ? NLMS_TAPS - 1
                                           : history_index - 1;
                }
            }
        }

        filter->reference_position++;
        if (filter->reference_position == NLMS_TAPS)
            filter->reference_position = 0;
    }
}

static float run_rnnoise(struct voice_isolator_data *filter)
{
    float maximum_vad = 0.0f;

    if (filter->to_rnnoise) {
        float *resampled[VOICE_ISOLATOR_MAX_CHANNELS] = {0};
        uint32_t output_frames = 0;
        uint64_t timestamp_offset = 0;

        audio_resampler_resample(
            filter->to_rnnoise, (uint8_t **)resampled, &output_frames,
            &timestamp_offset, (const uint8_t **)filter->copy_buffers,
            (uint32_t)filter->segment_frames);

        for (size_t channel = 0; channel < filter->channels; channel++) {
            for (size_t frame = 0; frame < RNNOISE_FRAME_SIZE; frame++) {
                const ssize_t source_index =
                    (ssize_t)output_frames -
                    (ssize_t)RNNOISE_FRAME_SIZE + (ssize_t)frame;
                filter->rnnoise_buffers[channel][frame] =
                    source_index >= 0
                        ? resampled[channel][source_index] * 32768.0f
                        : 0.0f;
            }
        }
    } else {
        for (size_t channel = 0; channel < filter->channels; channel++) {
            for (size_t frame = 0; frame < RNNOISE_FRAME_SIZE; frame++) {
                filter->rnnoise_buffers[channel][frame] =
                    filter->copy_buffers[channel][frame] * 32768.0f;
            }
        }
    }

    for (size_t channel = 0; channel < filter->channels; channel++) {
        const float vad =
            rnnoise_process_frame(filter->rnnoise_states[channel],
                                  filter->rnnoise_buffers[channel],
                                  filter->rnnoise_buffers[channel]);
        if (vad > maximum_vad)
            maximum_vad = vad;
    }

    if (filter->from_rnnoise) {
        float *resampled[VOICE_ISOLATOR_MAX_CHANNELS] = {0};
        uint32_t output_frames = 0;
        uint64_t timestamp_offset = 0;

        audio_resampler_resample(
            filter->from_rnnoise, (uint8_t **)resampled, &output_frames,
            &timestamp_offset,
            (const uint8_t **)filter->rnnoise_buffers,
            RNNOISE_FRAME_SIZE);

        for (size_t channel = 0; channel < filter->channels; channel++) {
            for (size_t frame = 0; frame < filter->segment_frames;
                 frame++) {
                const ssize_t source_index =
                    (ssize_t)output_frames -
                    (ssize_t)filter->segment_frames +
                    (ssize_t)frame;
                filter->copy_buffers[channel][frame] =
                    source_index >= 0
                        ? resampled[channel][source_index] / 32768.0f
                        : 0.0f;
            }
        }
    } else {
        for (size_t channel = 0; channel < filter->channels; channel++) {
            for (size_t frame = 0; frame < RNNOISE_FRAME_SIZE; frame++) {
                filter->copy_buffers[channel][frame] =
                    filter->rnnoise_buffers[channel][frame] / 32768.0f;
            }
        }
    }

    return clampf(maximum_vad, 0.0f, 1.0f);
}

static void calculate_frame_features(struct voice_isolator_data *filter,
                                     float *rms_db,
                                     float *derivative_ratio)
{
    double energy = 0.0;
    double derivative_energy = 0.0;
    float previous = 0.0f;

    for (size_t frame = 0; frame < filter->segment_frames; frame++) {
        float sample = 0.0f;

        for (size_t channel = 0; channel < filter->channels; channel++)
            sample += filter->copy_buffers[channel][frame];

        sample /= (float)filter->channels;
        energy += (double)sample * (double)sample;

        const float difference = sample - previous;
        derivative_energy +=
            (double)difference * (double)difference;
        previous = sample;
    }

    const double denominator =
        (double)(filter->segment_frames > 0
                     ? filter->segment_frames
                     : 1);
    const double mean_square = energy / denominator;

    *rms_db =
        (float)(10.0 * log10(mean_square + 1.0e-12));
    *derivative_ratio =
        (float)(derivative_energy / (energy + 1.0e-12));
}

static void apply_voice_gate(struct voice_isolator_data *filter,
                             float vad_probability)
{
    float rms_db = -120.0f;
    float derivative_ratio = 0.0f;
    calculate_frame_features(filter, &rms_db, &derivative_ratio);

    const bool speech =
        vad_probability >= filter->voice_threshold;
    const bool breath_candidate =
        !speech &&
        rms_db > filter->noise_floor_db &&
        derivative_ratio > 0.55f;

    float target_gain = 0.0f;

    if (speech) {
        target_gain = 1.0f;
        filter->hangover_segments =
            filter->gate_release_ms / SEGMENT_MS;
    } else if (breath_candidate) {
        target_gain =
            clampf(1.0f - filter->breath_suppression, 0.0f,
                   0.35f);
        if (filter->hangover_segments > 0)
            filter->hangover_segments--;
    } else if (filter->hangover_segments > 0) {
        const float relative_vad =
            filter->voice_threshold > 0.0f
                ? vad_probability / filter->voice_threshold
                : 0.0f;
        target_gain =
            clampf(relative_vad, 0.10f, 0.65f);
        filter->hangover_segments--;
    } else {
        target_gain = 0.0f;
    }

    if (rms_db < filter->noise_floor_db && !speech)
        target_gain = 0.0f;

    const float start_gain = filter->gate_gain;

    for (size_t frame = 0; frame < filter->segment_frames; frame++) {
        const float progress =
            (float)(frame + 1) /
            (float)(filter->segment_frames > 0
                        ? filter->segment_frames
                        : 1);
        const float gain =
            start_gain + (target_gain - start_gain) * progress;

        for (size_t channel = 0; channel < filter->channels; channel++) {
            float sample =
                filter->copy_buffers[channel][frame] * gain;
            filter->copy_buffers[channel][frame] =
                clampf(sample, -1.0f, 1.0f);
        }
    }

    filter->gate_gain = target_gain;
    filter->last_vad = vad_probability;
}

static void process_segment(struct voice_isolator_data *filter)
{
    const size_t segment_bytes =
        filter->segment_frames * sizeof(float);

    for (size_t channel = 0; channel < filter->channels; channel++) {
        deque_pop_front(&filter->input_buffers[channel],
                        filter->copy_buffers[channel], segment_bytes);
    }

    if (filter->aux_input_buffer.size >= segment_bytes) {
        deque_pop_front(&filter->aux_input_buffer,
                        filter->segment_aux, segment_bytes);
    } else {
        memset(filter->segment_aux, 0, segment_bytes);
    }

    adaptive_reference_cancel(filter);
    const float vad_probability = run_rnnoise(filter);
    apply_voice_gate(filter, vad_probability);

    for (size_t channel = 0; channel < filter->channels; channel++) {
        deque_push_back(&filter->output_buffers[channel],
                        filter->copy_buffers[channel], segment_bytes);
    }
}

static void ensure_output_capacity(struct voice_isolator_data *filter,
                                   size_t frames)
{
    const size_t required = frames * filter->channels;

    if (required <= filter->output_capacity_floats)
        return;

    float *replacement =
        brealloc(filter->output_storage,
                 required * sizeof(float));

    if (!replacement)
        return;

    filter->output_storage = replacement;
    filter->output_capacity_floats = required;
}

static struct obs_audio_data *
voice_isolator_filter_audio(void *data,
                            struct obs_audio_data *audio)
{
    struct voice_isolator_data *filter = data;

    if (!filter || !audio || audio->frames == 0)
        return audio;

    if (filter->last_timestamp != 0) {
        const int64_t difference =
            llabs((int64_t)filter->last_timestamp -
                  (int64_t)audio->timestamp);

        if (difference > (int64_t)NS_PER_SECOND)
            reset_processing_buffers(filter);
    }

    filter->last_timestamp = audio->timestamp;

    struct packet_info info = {
        .frames = audio->frames,
        .timestamp = audio->timestamp,
    };
    deque_push_back(&filter->info_buffer, &info, sizeof(info));

    for (size_t channel = 0; channel < filter->channels; channel++) {
        if (audio->data[channel]) {
            deque_push_back(&filter->input_buffers[channel],
                            audio->data[channel],
                            audio->frames * sizeof(float));
        } else {
            if (audio->frames > filter->aux_packet_capacity) {
                float *replacement =
                    brealloc(filter->aux_packet_buffer,
                             audio->frames * sizeof(float));
                if (replacement) {
                    filter->aux_packet_buffer = replacement;
                    filter->aux_packet_capacity = audio->frames;
                }
            }

            if (filter->aux_packet_buffer) {
                memset(filter->aux_packet_buffer, 0,
                       audio->frames * sizeof(float));
                deque_push_back(&filter->input_buffers[channel],
                                filter->aux_packet_buffer,
                                audio->frames * sizeof(float));
            }
        }
    }

    if (audio->frames > filter->aux_packet_capacity) {
        float *replacement =
            brealloc(filter->aux_packet_buffer,
                     audio->frames * sizeof(float));

        if (replacement) {
            filter->aux_packet_buffer = replacement;
            filter->aux_packet_capacity = audio->frames;
        }
    }

    if (filter->aux_packet_buffer) {
        read_aligned_auxiliary(
            filter, audio->timestamp, audio->frames,
            filter->aux_packet_buffer);
        deque_push_back(&filter->aux_input_buffer,
                        filter->aux_packet_buffer,
                        audio->frames * sizeof(float));
    }

    const size_t segment_bytes =
        filter->segment_frames * sizeof(float);

    while (filter->input_buffers[0].size >= segment_bytes)
        process_segment(filter);

    struct packet_info output_info = {0};
    deque_peek_front(&filter->info_buffer, &output_info,
                     sizeof(output_info));

    const size_t output_bytes =
        output_info.frames * sizeof(float);

    if (filter->output_buffers[0].size < output_bytes)
        return NULL;

    deque_pop_front(&filter->info_buffer, NULL,
                    sizeof(output_info));

    ensure_output_capacity(filter, output_info.frames);
    if (!filter->output_storage)
        return NULL;

    for (size_t channel = 0; channel < filter->channels; channel++) {
        float *destination =
            filter->output_storage +
            channel * output_info.frames;
        filter->output_audio.data[channel] =
            (uint8_t *)destination;

        deque_pop_front(&filter->output_buffers[channel],
                        destination, output_bytes);
    }

    for (size_t channel = filter->channels;
         channel < MAX_AV_PLANES; channel++) {
        filter->output_audio.data[channel] = NULL;
    }

    filter->output_audio.frames = output_info.frames;
    filter->output_audio.timestamp =
        output_info.timestamp > filter->latency_ns
            ? output_info.timestamp - filter->latency_ns
            : 0;

    return &filter->output_audio;
}

static void allocate_processing_state(
    struct voice_isolator_data *filter)
{
    filter->sample_rate =
        audio_output_get_sample_rate(obs_get_audio());
    filter->channels =
        audio_output_get_channels(obs_get_audio());

    if (filter->channels == 0)
        filter->channels = 1;
    if (filter->channels > VOICE_ISOLATOR_MAX_CHANNELS)
        filter->channels = VOICE_ISOLATOR_MAX_CHANNELS;

    filter->segment_frames =
        filter->sample_rate / (1000 / SEGMENT_MS);
    filter->latency_ns =
        NS_PER_SECOND / (1000 / SEGMENT_MS);

    filter->copy_storage =
        bzalloc(filter->segment_frames * filter->channels *
                sizeof(float));
    filter->rnnoise_storage =
        bzalloc(RNNOISE_FRAME_SIZE * filter->channels *
                sizeof(float));
    filter->segment_aux =
        bzalloc(filter->segment_frames * sizeof(float));
    filter->nlms_weights =
        bzalloc(filter->channels * NLMS_TAPS *
                sizeof(float));

    for (size_t channel = 0; channel < filter->channels; channel++) {
        filter->copy_buffers[channel] =
            filter->copy_storage +
            channel * filter->segment_frames;
        filter->rnnoise_buffers[channel] =
            filter->rnnoise_storage +
            channel * RNNOISE_FRAME_SIZE;
        filter->rnnoise_states[channel] =
            rnnoise_create(NULL);

        deque_reserve(
            &filter->input_buffers[channel],
            filter->segment_frames * sizeof(float) * 4);
        deque_reserve(
            &filter->output_buffers[channel],
            filter->segment_frames * sizeof(float) * 4);
    }

    deque_reserve(
        &filter->aux_input_buffer,
        filter->segment_frames * sizeof(float) * 4);
    deque_reserve(
        &filter->aux_samples,
        (size_t)filter->sample_rate * sizeof(float));

    if (filter->sample_rate != RNNOISE_SAMPLE_RATE) {
        struct resample_info source = {
            .samples_per_sec = filter->sample_rate,
            .format = AUDIO_FORMAT_FLOAT_PLANAR,
            .speakers =
                speaker_layout_from_channels(filter->channels),
        };
        struct resample_info rnnoise = {
            .samples_per_sec = RNNOISE_SAMPLE_RATE,
            .format = AUDIO_FORMAT_FLOAT_PLANAR,
            .speakers =
                speaker_layout_from_channels(filter->channels),
        };

        filter->to_rnnoise =
            audio_resampler_create(&rnnoise, &source);
        filter->from_rnnoise =
            audio_resampler_create(&source, &rnnoise);
    }
}

static void voice_isolator_update(void *data,
                                  obs_data_t *settings)
{
    struct voice_isolator_data *filter = data;

    filter->voice_threshold =
        (float)obs_data_get_double(
            settings, S_VOICE_THRESHOLD);
    filter->breath_suppression =
        (float)obs_data_get_double(
            settings, S_BREATH_SUPPRESSION);
    filter->noise_floor_db =
        (float)obs_data_get_double(
            settings, S_NOISE_FLOOR_DB);
    filter->aux_cancellation =
        (float)obs_data_get_double(
            settings, S_AUX_CANCELLATION);
    filter->aux_adapt_rate =
        (float)obs_data_get_double(
            settings, S_AUX_ADAPT_RATE);
    filter->gate_release_ms =
        (int)obs_data_get_int(
            settings, S_GATE_RELEASE_MS);
    filter->aux_offset_ms =
        (int)obs_data_get_int(
            settings, S_AUX_OFFSET_MS);

    attach_auxiliary_source(
        filter,
        obs_data_get_string(settings, S_AUX_SOURCE));
}

static void *voice_isolator_create(obs_data_t *settings,
                                   obs_source_t *source)
{
    struct voice_isolator_data *filter =
        bzalloc(sizeof(*filter));

    filter->context = source;
    pthread_mutex_init(&filter->aux_mutex, NULL);
    allocate_processing_state(filter);
    voice_isolator_update(filter, settings);

    return filter;
}

static void voice_isolator_destroy(void *data)
{
    struct voice_isolator_data *filter = data;

    if (!filter)
        return;

    detach_auxiliary_source(filter);

    for (size_t channel = 0; channel < filter->channels; channel++) {
        if (filter->rnnoise_states[channel])
            rnnoise_destroy(filter->rnnoise_states[channel]);

        deque_free(&filter->input_buffers[channel]);
        deque_free(&filter->output_buffers[channel]);
    }

    if (filter->to_rnnoise)
        audio_resampler_destroy(filter->to_rnnoise);
    if (filter->from_rnnoise)
        audio_resampler_destroy(filter->from_rnnoise);

    deque_free(&filter->info_buffer);
    deque_free(&filter->aux_input_buffer);
    deque_free(&filter->aux_samples);

    bfree(filter->copy_storage);
    bfree(filter->rnnoise_storage);
    bfree(filter->segment_aux);
    bfree(filter->nlms_weights);
    bfree(filter->output_storage);
    bfree(filter->aux_callback_buffer);
    bfree(filter->aux_packet_buffer);
    bfree(filter->aux_source_name);

    pthread_mutex_destroy(&filter->aux_mutex);
    bfree(filter);
}

static const char *voice_isolator_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("VoiceIsolator.Name");
}

struct source_list_context {
    obs_property_t *property;
    const char *parent_name;
};

static bool add_audio_source_to_list(void *parameter,
                                     obs_source_t *source)
{
    struct source_list_context *context = parameter;
    const char *name = obs_source_get_name(source);
    const uint32_t flags = obs_source_get_output_flags(source);

    if (!name || !*name || !(flags & OBS_SOURCE_AUDIO))
        return true;

    if (context->parent_name &&
        strcmp(name, context->parent_name) == 0)
        return true;

    obs_property_list_add_string(
        context->property, name, name);
    return true;
}

static obs_properties_t *
voice_isolator_properties(void *data)
{
    struct voice_isolator_data *filter = data;
    obs_properties_t *properties = obs_properties_create();

    obs_properties_add_text(
        properties, "notice",
        obs_module_text("VoiceIsolator.Notice"),
        OBS_TEXT_INFO);

    obs_properties_add_float_slider(
        properties, S_VOICE_THRESHOLD,
        obs_module_text("VoiceIsolator.VoiceThreshold"),
        0.10, 0.95, 0.01);

    obs_properties_add_float_slider(
        properties, S_BREATH_SUPPRESSION,
        obs_module_text("VoiceIsolator.BreathSuppression"),
        0.0, 1.0, 0.01);

    obs_properties_add_int_slider(
        properties, S_GATE_RELEASE_MS,
        obs_module_text("VoiceIsolator.GateRelease"),
        20, 500, 10);

    obs_property_t *floor_property =
        obs_properties_add_float_slider(
            properties, S_NOISE_FLOOR_DB,
            obs_module_text("VoiceIsolator.NoiseFloor"),
            -80.0, -20.0, 1.0);
    obs_property_float_set_suffix(floor_property, " dB");

    obs_property_t *auxiliary =
        obs_properties_add_list(
            properties, S_AUX_SOURCE,
            obs_module_text("VoiceIsolator.AuxSource"),
            OBS_COMBO_TYPE_LIST,
            OBS_COMBO_FORMAT_STRING);

    obs_property_list_add_string(
        auxiliary,
        obs_module_text("VoiceIsolator.AuxDisabled"), "");

    struct source_list_context context = {
        .property = auxiliary,
        .parent_name = NULL,
    };

    if (filter) {
        obs_source_t *parent =
            obs_filter_get_parent(filter->context);
        if (parent)
            context.parent_name =
                obs_source_get_name(parent);
    }

    obs_enum_sources(add_audio_source_to_list, &context);

    obs_properties_add_float_slider(
        properties, S_AUX_CANCELLATION,
        obs_module_text("VoiceIsolator.AuxCancellation"),
        0.0, 1.0, 0.01);

    obs_properties_add_float_slider(
        properties, S_AUX_ADAPT_RATE,
        obs_module_text("VoiceIsolator.AuxAdaptRate"),
        0.001, 0.20, 0.001);

    obs_property_t *offset_property =
        obs_properties_add_int_slider(
            properties, S_AUX_OFFSET_MS,
            obs_module_text("VoiceIsolator.AuxOffset"),
            -200, 200, 1);
    obs_property_int_set_suffix(offset_property, " ms");

    return properties;
}

static void voice_isolator_defaults(obs_data_t *settings)
{
    obs_data_set_default_double(
        settings, S_VOICE_THRESHOLD, 0.55);
    obs_data_set_default_double(
        settings, S_BREATH_SUPPRESSION, 0.95);
    obs_data_set_default_int(
        settings, S_GATE_RELEASE_MS, 140);
    obs_data_set_default_double(
        settings, S_NOISE_FLOOR_DB, -58.0);
    obs_data_set_default_string(
        settings, S_AUX_SOURCE, "");
    obs_data_set_default_double(
        settings, S_AUX_CANCELLATION, 0.65);
    obs_data_set_default_double(
        settings, S_AUX_ADAPT_RATE, 0.04);
    obs_data_set_default_int(
        settings, S_AUX_OFFSET_MS, 0);
}

struct obs_source_info voice_isolator_filter = {
    .id = "obs_voice_isolator_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = voice_isolator_name,
    .create = voice_isolator_create,
    .destroy = voice_isolator_destroy,
    .update = voice_isolator_update,
    .filter_audio = voice_isolator_filter_audio,
    .get_defaults = voice_isolator_defaults,
    .get_properties = voice_isolator_properties,
};

