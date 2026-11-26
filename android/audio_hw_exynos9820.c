// SPDX-License-Identifier: Apache-2.0
/*
 * Minimal Android Primary Audio HAL for Exynos 9820 mainline ABOX + Madera + CS35L40.
 * Opens ALSA card 0 device 1 (Speaker TDM) for playback and device 0 (Codec AIF1) for capture
 * via libtinyalsa.so, configuring the Cirrus CS35L40 and CS47L93 mixer controls.
 */

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>
#include <hardware/audio.h>
#include <hardware/hardware.h>

#define LOG_TAG "audio_hw_exynos9820"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* tinyalsa ABI types */
#define PCM_OUT 0x00000000
#define PCM_IN  0x10000000

enum pcm_format {
	PCM_FORMAT_S16_LE = 0,
	PCM_FORMAT_S32_LE = 1,
};

struct pcm_config {
	unsigned int channels;
	unsigned int rate;
	unsigned int period_size;
	unsigned int period_count;
	enum pcm_format format;
	unsigned int start_threshold;
	unsigned int stop_threshold;
	unsigned int silence_threshold;
	int avail_min;
};

struct pcm;
struct mixer;
struct mixer_ctl;

static struct pcm *(*fn_pcm_open)(unsigned int card, unsigned int device,
				  unsigned int flags, struct pcm_config *config);
static int (*fn_pcm_is_ready)(struct pcm *pcm);
static int (*fn_pcm_close)(struct pcm *pcm);
static int (*fn_pcm_write)(struct pcm *pcm, const void *data, unsigned int count);
static int (*fn_pcm_read)(struct pcm *pcm, void *data, unsigned int count);
static const char *(*fn_pcm_get_error)(struct pcm *pcm);
static struct mixer *(*fn_mixer_open)(unsigned int card);
static void (*fn_mixer_close)(struct mixer *mixer);
static struct mixer_ctl *(*fn_mixer_get_ctl_by_name)(struct mixer *mixer, const char *name);
static int (*fn_mixer_ctl_set_value)(struct mixer_ctl *ctl, unsigned int id, int value);
static int (*fn_mixer_ctl_set_enum_by_string)(struct mixer_ctl *ctl, const char *string);

static bool tinyalsa_loaded;

static int load_tinyalsa(void)
{
	void *h;

	if (tinyalsa_loaded)
		return 0;

	h = dlopen("libtinyalsa.so", RTLD_NOW);
	if (!h) {
		ALOGE("dlopen(libtinyalsa.so) failed: %s", dlerror());
		return -ENOENT;
	}

	fn_pcm_open = dlsym(h, "pcm_open");
	fn_pcm_is_ready = dlsym(h, "pcm_is_ready");
	fn_pcm_close = dlsym(h, "pcm_close");
	fn_pcm_write = dlsym(h, "pcm_write");
	fn_pcm_read = dlsym(h, "pcm_read");
	fn_pcm_get_error = dlsym(h, "pcm_get_error");
	fn_mixer_open = dlsym(h, "mixer_open");
	fn_mixer_close = dlsym(h, "mixer_close");
	fn_mixer_get_ctl_by_name = dlsym(h, "mixer_get_ctl_by_name");
	fn_mixer_ctl_set_value = dlsym(h, "mixer_ctl_set_value");
	fn_mixer_ctl_set_enum_by_string = dlsym(h, "mixer_ctl_set_enum_by_string");

	if (!fn_pcm_open || !fn_pcm_is_ready || !fn_pcm_close || !fn_pcm_write ||
	    !fn_pcm_read || !fn_mixer_open || !fn_mixer_close ||
	    !fn_mixer_get_ctl_by_name || !fn_mixer_ctl_set_value ||
	    !fn_mixer_ctl_set_enum_by_string) {
		ALOGE("missing required tinyalsa symbols");
		return -ENOSYS;
	}

	tinyalsa_loaded = true;
	return 0;
}

static void mixer_set_int(struct mixer *m, const char *name, int val)
{
	struct mixer_ctl *ctl = fn_mixer_get_ctl_by_name(m, name);

	if (ctl)
		fn_mixer_ctl_set_value(ctl, 0, val);
}

static void mixer_set_enum(struct mixer *m, const char *name, const char *val)
{
	struct mixer_ctl *ctl = fn_mixer_get_ctl_by_name(m, name);

	if (ctl)
		fn_mixer_ctl_set_enum_by_string(ctl, val);
}

static void setup_speaker_mixer(void)
{
	struct mixer *m = fn_mixer_open(0);

	if (!m)
		return;
	mixer_set_enum(m, "SPK PCM Source", "ASP");
	mixer_set_enum(m, "RCV PCM Source", "ASP");
	mixer_set_int(m, "SPK Analog PCM Volume", 17);
	mixer_set_int(m, "RCV Analog PCM Volume", 17);
	mixer_set_int(m, "SPK Digital PCM Volume", 769);
	mixer_set_int(m, "RCV Digital PCM Volume", 761);
	fn_mixer_close(m);
}

static void setup_mic_mixer(void)
{
	struct mixer *m = fn_mixer_open(0);

	if (!m)
		return;
	mixer_set_enum(m, "IN3 OSR", "3.072MHz");
	mixer_set_enum(m, "IN4 OSR", "3.072MHz");
	mixer_set_int(m, "IN3L HPF Switch", 1);
	mixer_set_int(m, "IN4L HPF Switch", 1);
	mixer_set_int(m, "IN3L Digital Volume", 176);
	mixer_set_int(m, "IN4L Digital Volume", 176);
	mixer_set_int(m, "Main Mic Switch", 1);
	mixer_set_int(m, "Sub Mic Switch", 1);
	mixer_set_enum(m, "AIF1TX1 Input 1", "IN3L");
	mixer_set_enum(m, "AIF1TX2 Input 1", "IN4L");
	fn_mixer_close(m);
}

struct exynos_audio_dev {
	struct audio_hw_device device;
	pthread_mutex_t lock;
	bool mic_mute;
};

struct exynos_stream_out {
	struct audio_stream_out stream;
	pthread_mutex_t lock;
	struct pcm *pcm;
	int32_t *s32_buf;
	size_t s32_cap;
	uint64_t written_frames;
	bool standby;
};

struct exynos_stream_in {
	struct audio_stream_in stream;
	pthread_mutex_t lock;
	struct pcm *pcm;
	bool standby;
};

static uint32_t out_get_sample_rate(const struct audio_stream *stream) { (void)stream; return 48000; }
static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate) { (void)stream; (void)rate; return 0; }
static size_t out_get_buffer_size(const struct audio_stream *stream) { (void)stream; return 1024 * 4; }
static audio_channel_mask_t out_get_channels(const struct audio_stream *stream) { (void)stream; return AUDIO_CHANNEL_OUT_STEREO; }
static audio_format_t out_get_format(const struct audio_stream *stream) { (void)stream; return AUDIO_FORMAT_PCM_16_BIT; }
static int out_set_format(struct audio_stream *stream, audio_format_t format) { (void)stream; (void)format; return 0; }
static int out_dump(const struct audio_stream *stream, int fd) { (void)stream; (void)fd; return 0; }
static int out_set_parameters(struct audio_stream *stream, const char *kvpairs) { (void)stream; (void)kvpairs; return 0; }
static char *out_get_parameters(const struct audio_stream *stream, const char *keys) { (void)stream; (void)keys; return strdup(""); }
static uint32_t out_get_latency(const struct audio_stream_out *stream) { (void)stream; return 40; }
static int out_set_volume(struct audio_stream_out *stream, float left, float right) { (void)stream; (void)left; (void)right; return -ENOSYS; }
static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect) { (void)stream; (void)effect; return 0; }
static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect) { (void)stream; (void)effect; return 0; }
static int out_get_render_position(const struct audio_stream_out *stream, uint32_t *dsp_frames)
{
	const struct exynos_stream_out *out = (const struct exynos_stream_out *)stream;
	if (dsp_frames)
		*dsp_frames = (uint32_t)out->written_frames;
	return 0;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
					 uint64_t *frames, struct timespec *timestamp)
{
	const struct exynos_stream_out *out = (const struct exynos_stream_out *)stream;
	if (frames)
		*frames = out->written_frames;
	if (timestamp)
		clock_gettime(CLOCK_MONOTONIC, timestamp);
	return 0;
}

static int out_standby(struct audio_stream *stream)
{
	struct exynos_stream_out *out = (struct exynos_stream_out *)stream;

	pthread_mutex_lock(&out->lock);
	if (out->pcm) {
		fn_pcm_close(out->pcm);
		out->pcm = NULL;
	}
	out->standby = true;
	pthread_mutex_unlock(&out->lock);
	return 0;
}

static ssize_t out_write(struct audio_stream_out *stream, const void *buffer, size_t bytes)
{
	struct exynos_stream_out *out = (struct exynos_stream_out *)stream;
	const int16_t *src = (const int16_t *)buffer;
	size_t samples = bytes / sizeof(int16_t);
	size_t i;
	int ret;

	pthread_mutex_lock(&out->lock);
	if (out->standby || !out->pcm) {
		struct pcm_config cfg = {
			.channels = 2,
			.rate = 48000,
			.period_size = 1024,
			.period_count = 4,
			.format = PCM_FORMAT_S32_LE,
		};

		setup_speaker_mixer();
		out->pcm = fn_pcm_open(0, 1, PCM_OUT, &cfg);
		if (!out->pcm || !fn_pcm_is_ready(out->pcm)) {
			ALOGE("pcm_open(0,1) failed: %s",
			      out->pcm && fn_pcm_get_error ? fn_pcm_get_error(out->pcm) : "null");
			if (out->pcm) {
				fn_pcm_close(out->pcm);
				out->pcm = NULL;
			}
			pthread_mutex_unlock(&out->lock);
			usleep((bytes * 1000000ULL) / (48000ULL * 4ULL));
			out->written_frames += bytes / 4;
			return (ssize_t)bytes;
		}
		out->standby = false;
		ALOGI("opened speaker PCM hw:0,1 (48kHz stereo S32_LE)");
	}

	if (samples > out->s32_cap) {
		int32_t *nbuf = realloc(out->s32_buf, samples * sizeof(int32_t));
		if (!nbuf) {
			pthread_mutex_unlock(&out->lock);
			return -ENOMEM;
		}
		out->s32_buf = nbuf;
		out->s32_cap = samples;
	}
	for (i = 0; i < samples; i++)
		out->s32_buf[i] = ((int32_t)src[i]) << 16;

	ret = fn_pcm_write(out->pcm, out->s32_buf, (unsigned int)(samples * sizeof(int32_t)));
	if (ret != 0) {
		ALOGE("pcm_write failed: %s", fn_pcm_get_error ? fn_pcm_get_error(out->pcm) : "err");
		fn_pcm_close(out->pcm);
		out->pcm = NULL;
		out->standby = true;
	}
	out->written_frames += bytes / 4;
	pthread_mutex_unlock(&out->lock);
	return (ssize_t)bytes;
}

static uint32_t in_get_sample_rate(const struct audio_stream *stream) { (void)stream; return 48000; }
static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate) { (void)stream; (void)rate; return 0; }
static size_t in_get_buffer_size(const struct audio_stream *stream) { (void)stream; return 1024 * 4; }
static audio_channel_mask_t in_get_channels(const struct audio_stream *stream) { (void)stream; return AUDIO_CHANNEL_IN_STEREO; }
static audio_format_t in_get_format(const struct audio_stream *stream) { (void)stream; return AUDIO_FORMAT_PCM_16_BIT; }
static int in_set_format(struct audio_stream *stream, audio_format_t format) { (void)stream; (void)format; return 0; }
static int in_dump(const struct audio_stream *stream, int fd) { (void)stream; (void)fd; return 0; }
static int in_set_parameters(struct audio_stream *stream, const char *kvpairs) { (void)stream; (void)kvpairs; return 0; }
static char *in_get_parameters(const struct audio_stream *stream, const char *keys) { (void)stream; (void)keys; return strdup(""); }
static int in_set_gain(struct audio_stream_in *stream, float gain) { (void)stream; (void)gain; return 0; }
static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream) { (void)stream; return 0; }
static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect) { (void)stream; (void)effect; return 0; }
static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect) { (void)stream; (void)effect; return 0; }

static int in_standby(struct audio_stream *stream)
{
	struct exynos_stream_in *in = (struct exynos_stream_in *)stream;

	pthread_mutex_lock(&in->lock);
	if (in->pcm) {
		fn_pcm_close(in->pcm);
		in->pcm = NULL;
	}
	in->standby = true;
	pthread_mutex_unlock(&in->lock);
	return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void *buffer, size_t bytes)
{
	struct exynos_stream_in *in = (struct exynos_stream_in *)stream;

	pthread_mutex_lock(&in->lock);
	if (in->standby || !in->pcm) {
		struct pcm_config cfg = {
			.channels = 2,
			.rate = 48000,
			.period_size = 1024,
			.period_count = 4,
			.format = PCM_FORMAT_S16_LE,
		};

		setup_mic_mixer();
		in->pcm = fn_pcm_open(0, 0, PCM_IN, &cfg);
		if (!in->pcm || !fn_pcm_is_ready(in->pcm)) {
			if (in->pcm) {
				fn_pcm_close(in->pcm);
				in->pcm = NULL;
			}
			memset(buffer, 0, bytes);
			pthread_mutex_unlock(&in->lock);
			usleep((bytes * 1000000ULL) / (48000ULL * 4ULL));
			return (ssize_t)bytes;
		}
		in->standby = false;
	}

	if (fn_pcm_read(in->pcm, buffer, (unsigned int)bytes) != 0)
		memset(buffer, 0, bytes);
	pthread_mutex_unlock(&in->lock);
	return (ssize_t)bytes;
}

static int adev_open_output_stream(struct audio_hw_device *dev, audio_io_handle_t handle,
				   audio_devices_t devices, audio_output_flags_t flags,
				   struct audio_config *config,
				   struct audio_stream_out **stream_out, const char *address)
{
	struct exynos_stream_out *out;
	(void)dev; (void)handle; (void)devices; (void)flags; (void)address;

	out = calloc(1, sizeof(*out));
	if (!out)
		return -ENOMEM;

	pthread_mutex_init(&out->lock, NULL);
	out->standby = true;

	out->stream.common.get_sample_rate = out_get_sample_rate;
	out->stream.common.set_sample_rate = out_set_sample_rate;
	out->stream.common.get_buffer_size = out_get_buffer_size;
	out->stream.common.get_channels = out_get_channels;
	out->stream.common.get_format = out_get_format;
	out->stream.common.set_format = out_set_format;
	out->stream.common.standby = out_standby;
	out->stream.common.dump = out_dump;
	out->stream.common.set_parameters = out_set_parameters;
	out->stream.common.get_parameters = out_get_parameters;
	out->stream.common.add_audio_effect = out_add_audio_effect;
	out->stream.common.remove_audio_effect = out_remove_audio_effect;
	out->stream.get_latency = out_get_latency;
	out->stream.set_volume = out_set_volume;
	out->stream.write = out_write;
	out->stream.get_render_position = out_get_render_position;
	out->stream.get_presentation_position = out_get_presentation_position;

	if (config) {
		config->sample_rate = 48000;
		config->channel_mask = AUDIO_CHANNEL_OUT_STEREO;
		config->format = AUDIO_FORMAT_PCM_16_BIT;
	}
	*stream_out = &out->stream;
	return 0;
}

static void adev_close_output_stream(struct audio_hw_device *dev, struct audio_stream_out *stream)
{
	struct exynos_stream_out *out = (struct exynos_stream_out *)stream;
	(void)dev;

	out_standby(&stream->common);
	free(out->s32_buf);
	pthread_mutex_destroy(&out->lock);
	free(out);
}

static int adev_open_input_stream(struct audio_hw_device *dev, audio_io_handle_t handle,
				  audio_devices_t devices, struct audio_config *config,
				  struct audio_stream_in **stream_in,
				  audio_input_flags_t flags, const char *address,
				  audio_source_t source)
{
	struct exynos_stream_in *in;
	(void)dev; (void)handle; (void)devices; (void)flags; (void)address; (void)source;

	in = calloc(1, sizeof(*in));
	if (!in)
		return -ENOMEM;

	pthread_mutex_init(&in->lock, NULL);
	in->standby = true;

	in->stream.common.get_sample_rate = in_get_sample_rate;
	in->stream.common.set_sample_rate = in_set_sample_rate;
	in->stream.common.get_buffer_size = in_get_buffer_size;
	in->stream.common.get_channels = in_get_channels;
	in->stream.common.get_format = in_get_format;
	in->stream.common.set_format = in_set_format;
	in->stream.common.standby = in_standby;
	in->stream.common.dump = in_dump;
	in->stream.common.set_parameters = in_set_parameters;
	in->stream.common.get_parameters = in_get_parameters;
	in->stream.common.add_audio_effect = in_add_audio_effect;
	in->stream.common.remove_audio_effect = in_remove_audio_effect;
	in->stream.set_gain = in_set_gain;
	in->stream.read = in_read;
	in->stream.get_input_frames_lost = in_get_input_frames_lost;

	if (config) {
		config->sample_rate = 48000;
		config->channel_mask = AUDIO_CHANNEL_IN_STEREO;
		config->format = AUDIO_FORMAT_PCM_16_BIT;
	}
	*stream_in = &in->stream;
	return 0;
}

static void adev_close_input_stream(struct audio_hw_device *dev, struct audio_stream_in *stream)
{
	struct exynos_stream_in *in = (struct exynos_stream_in *)stream;
	(void)dev;

	in_standby(&stream->common);
	pthread_mutex_destroy(&in->lock);
	free(in);
}

static int adev_init_check(const struct audio_hw_device *dev) { (void)dev; return 0; }
static int adev_set_voice_volume(struct audio_hw_device *dev, float volume) { (void)dev; (void)volume; return 0; }
static int adev_set_master_volume(struct audio_hw_device *dev, float volume) { (void)dev; (void)volume; return -ENOSYS; }
static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode) { (void)dev; (void)mode; return 0; }
static int adev_set_mic_mute(struct audio_hw_device *dev, bool state) { ((struct exynos_audio_dev *)dev)->mic_mute = state; return 0; }
static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state) { *state = ((const struct exynos_audio_dev *)dev)->mic_mute; return 0; }
static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs) { (void)dev; (void)kvpairs; return 0; }
static char *adev_get_parameters(const struct audio_hw_device *dev, const char *keys) { (void)dev; (void)keys; return strdup(""); }
static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev, const struct audio_config *config) { (void)dev; (void)config; return 1024 * 4; }
static int adev_dump(const struct audio_hw_device *dev, int fd) { (void)dev; (void)fd; return 0; }

static int adev_close(hw_device_t *device)
{
	struct exynos_audio_dev *adev = (struct exynos_audio_dev *)device;

	pthread_mutex_destroy(&adev->lock);
	free(adev);
	return 0;
}

static int adev_open(const hw_module_t *module, const char *name, hw_device_t **device)
{
	struct exynos_audio_dev *adev;

	if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
		return -EINVAL;
	if (load_tinyalsa() != 0)
		return -ENODEV;

	adev = calloc(1, sizeof(*adev));
	if (!adev)
		return -ENOMEM;

	pthread_mutex_init(&adev->lock, NULL);
	adev->device.common.tag = HARDWARE_DEVICE_TAG;
	adev->device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
	adev->device.common.module = (struct hw_module_t *)module;
	adev->device.common.close = adev_close;

	adev->device.init_check = adev_init_check;
	adev->device.set_voice_volume = adev_set_voice_volume;
	adev->device.set_master_volume = adev_set_master_volume;
	adev->device.set_mode = adev_set_mode;
	adev->device.set_mic_mute = adev_set_mic_mute;
	adev->device.get_mic_mute = adev_get_mic_mute;
	adev->device.set_parameters = adev_set_parameters;
	adev->device.get_parameters = adev_get_parameters;
	adev->device.get_input_buffer_size = adev_get_input_buffer_size;
	adev->device.open_output_stream = adev_open_output_stream;
	adev->device.close_output_stream = adev_close_output_stream;
	adev->device.open_input_stream = adev_open_input_stream;
	adev->device.close_input_stream = adev_close_input_stream;
	adev->device.dump = adev_dump;

	*device = &adev->device.common;
	ALOGI("Exynos 9820 mainline ABOX primary audio HAL opened");
	return 0;
}

static struct hw_module_methods_t hal_module_methods = {
	.open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
	.common = {
		.tag = HARDWARE_MODULE_TAG,
		.module_api_version = AUDIO_MODULE_API_VERSION_0_1,
		.hal_api_version = HARDWARE_HAL_API_VERSION,
		.id = AUDIO_HARDWARE_MODULE_ID,
		.name = "Exynos 9820 Mainline ABOX Audio HAL",
		.author = "exynos9820-mainline-android",
		.methods = &hal_module_methods,
	},
};
