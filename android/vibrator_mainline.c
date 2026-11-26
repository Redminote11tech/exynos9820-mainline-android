/*
 * Minimal mainline Android AIDL IVibrator v2 HAL for Exynos 9820 / Galaxy S10+
 * Drives Linux input force-feedback (FF_RUMBLE) on max77693-haptic (/dev/input/event0).
 *
 * Links only against LLNDK/Bionic libraries: libbinder_ndk.so, liblog.so, libc.so.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>

#define LOG_TAG "vibrator-mainline"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Platform/LLNDK symbols from libbinder_ndk.so (not in public NDK headers) */
extern binder_status_t AServiceManager_addService(AIBinder* binder, const char* instance);
extern void ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
extern void ABinderProcess_startThreadPool(void);
extern void ABinderProcess_joinThreadPool(void);

#define VIBRATOR_DESCRIPTOR "android.hardware.vibrator.IVibrator"
#define VIBRATOR_INSTANCE   "android.hardware.vibrator.IVibrator/default"
#define VIBRATOR_VERSION    2
#define VIBRATOR_HASH       "ea8742d6993e1a82917da38b9938e537aa7fcb54"

/* IVibrator v2 capability bitmask */
#define CAP_ON_CALLBACK       (1 << 0)
#define CAP_PERFORM_CALLBACK  (1 << 1)
#define CAP_AMPLITUDE_CONTROL (1 << 2)

/* IVibrator v2 transaction codes (FIRST_CALL_TRANSACTION = 1) */
enum {
    TX_GET_CAPABILITIES              = FIRST_CALL_TRANSACTION + 0,  /* 1 */
    TX_OFF                           = FIRST_CALL_TRANSACTION + 1,  /* 2 */
    TX_ON                            = FIRST_CALL_TRANSACTION + 2,  /* 3 */
    TX_PERFORM                       = FIRST_CALL_TRANSACTION + 3,  /* 4 */
    TX_GET_SUPPORTED_EFFECTS         = FIRST_CALL_TRANSACTION + 4,  /* 5 */
    TX_SET_AMPLITUDE                 = FIRST_CALL_TRANSACTION + 5,  /* 6 */
    TX_SET_EXTERNAL_CONTROL          = FIRST_CALL_TRANSACTION + 6,  /* 7 */
    TX_GET_COMPOSITION_DELAY_MAX     = FIRST_CALL_TRANSACTION + 7,  /* 8 */
    TX_GET_COMPOSITION_SIZE_MAX      = FIRST_CALL_TRANSACTION + 8,  /* 9 */
    TX_GET_SUPPORTED_PRIMITIVES      = FIRST_CALL_TRANSACTION + 9,  /* 10 */
    TX_GET_PRIMITIVE_DURATION        = FIRST_CALL_TRANSACTION + 10, /* 11 */
    TX_COMPOSE                       = FIRST_CALL_TRANSACTION + 11, /* 12 */
    TX_GET_SUPPORTED_ALWAYS_ON       = FIRST_CALL_TRANSACTION + 12, /* 13 */
    TX_ALWAYS_ON_ENABLE              = FIRST_CALL_TRANSACTION + 13, /* 14 */
    TX_ALWAYS_ON_DISABLE             = FIRST_CALL_TRANSACTION + 14, /* 15 */
    TX_GET_RESONANT_FREQUENCY        = FIRST_CALL_TRANSACTION + 15, /* 16 */
    TX_GET_Q_FACTOR                  = FIRST_CALL_TRANSACTION + 16, /* 17 */
    TX_GET_FREQUENCY_RESOLUTION      = FIRST_CALL_TRANSACTION + 17, /* 18 */
    TX_GET_FREQUENCY_MINIMUM         = FIRST_CALL_TRANSACTION + 18, /* 19 */
    TX_GET_BANDWIDTH_AMPLITUDE_MAP   = FIRST_CALL_TRANSACTION + 19, /* 20 */
    TX_GET_PWLE_PRIMITIVE_DUR_MAX    = FIRST_CALL_TRANSACTION + 20, /* 21 */
    TX_GET_PWLE_COMPOSITION_SIZE_MAX = FIRST_CALL_TRANSACTION + 21, /* 22 */
    TX_GET_SUPPORTED_BRAKING         = FIRST_CALL_TRANSACTION + 22, /* 23 */
    TX_COMPOSE_PWLE                  = FIRST_CALL_TRANSACTION + 23, /* 24 */
    TX_GET_INTERFACE_HASH            = FIRST_CALL_TRANSACTION + 16777213, /* 0x00fffffe */
    TX_GET_INTERFACE_VERSION         = FIRST_CALL_TRANSACTION + 16777214, /* 0x00ffffff */
};

/* IVibratorCallback transaction code */
#define CB_TX_ON_COMPLETE (FIRST_CALL_TRANSACTION + 0)

/* Standard Effect enum values */
enum {
    EFFECT_CLICK        = 0,
    EFFECT_DOUBLE_CLICK = 1,
    EFFECT_TICK         = 2,
    EFFECT_THUD         = 3,
    EFFECT_POP          = 4,
    EFFECT_HEAVY_CLICK  = 5,
    EFFECT_TEXTURE_TICK = 21,
};

static const int32_t kSupportedEffects[] = {
    EFFECT_CLICK,
    EFFECT_DOUBLE_CLICK,
    EFFECT_TICK,
    EFFECT_THUD,
    EFFECT_POP,
    EFFECT_HEAVY_CLICK,
    EFFECT_TEXTURE_TICK,
};

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int fd;
    int effect_id;
    float amplitude;
    uint64_t generation;
} VibratorState;

typedef struct {
    VibratorState* state;
    AIBinder* callback;
    int32_t timeout_ms;
    uint64_t generation;
} CallbackTask;

static bool test_bit(const unsigned long* bits, int bit) {
    return (bits[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1UL;
}

static bool has_ff_rumble(int fd) {
    unsigned long ev_bits[(EV_MAX / (8 * sizeof(unsigned long))) + 1];
    unsigned long ff_bits[(FF_MAX / (8 * sizeof(unsigned long))) + 1];
    memset(ev_bits, 0, sizeof(ev_bits));
    memset(ff_bits, 0, sizeof(ff_bits));

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
        return false;
    if (!test_bit(ev_bits, EV_FF))
        return false;
    if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff_bits)), ff_bits) < 0)
        return false;
    return test_bit(ff_bits, FF_RUMBLE);
}

static int open_haptic_device(void) {
    char path[64];
    char name[128];
    int fallback_fd = -1;

    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) {
            if (strstr(name, "max77693-haptic") != NULL || strstr(name, "haptic") != NULL) {
                ALOGI("Opened haptic device %s (%s)", path, name);
                if (fallback_fd >= 0)
                    close(fallback_fd);
                return fd;
            }
        }

        if (fallback_fd < 0 && has_ff_rumble(fd)) {
            fallback_fd = fd;
            ALOGI("Found fallback FF_RUMBLE device %s (%s)", path, name);
        } else {
            close(fd);
        }
    }

    if (fallback_fd >= 0)
        return fallback_fd;

    /* Final direct fallback */
    int fd = open("/dev/input/event0", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        ALOGW("Using fallback /dev/input/event0");
    } else {
        ALOGE("Failed to open any haptic input device: %s", strerror(errno));
    }
    return fd;
}

static int ensure_open_locked(VibratorState* st) {
    if (st->fd >= 0)
        return 0;
    st->fd = open_haptic_device();
    st->effect_id = -1;
    return (st->fd >= 0) ? 0 : -1;
}

static void send_ff_gain_locked(VibratorState* st, float amp) {
    if (st->fd < 0)
        return;
    struct input_event gain;
    memset(&gain, 0, sizeof(gain));
    gain.type = EV_FF;
    gain.code = FF_GAIN;
    gain.value = (int32_t)(amp * 0xFFFF);
    if (write(st->fd, &gain, sizeof(gain)) < 0) {
        ALOGW("FF_GAIN write failed: %s", strerror(errno));
    }
}

static int vibrator_off_locked(VibratorState* st) {
    st->generation++;
    pthread_cond_broadcast(&st->cond);

    if (st->fd < 0 || st->effect_id < 0)
        return 0;

    struct input_event play;
    memset(&play, 0, sizeof(play));
    play.type = EV_FF;
    play.code = (uint16_t)st->effect_id;
    play.value = 0;

    if (write(st->fd, &play, sizeof(play)) < 0) {
        ALOGW("EV_FF stop failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void* cb_on_create(void* args) { return args; }
static void cb_on_destroy(void* userData) { (void)userData; }
static binder_status_t cb_on_transact(AIBinder* b, transaction_code_t c, const AParcel* in, AParcel* out) {
    (void)b; (void)c; (void)in; (void)out;
    return STATUS_UNKNOWN_TRANSACTION;
}
static AIBinder_Class* g_cb_class = NULL;

static void* callback_thread_fn(void* arg) {
    CallbackTask* task = (CallbackTask*)arg;
    VibratorState* st = task->state;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += task->timeout_ms / 1000;
    ts.tv_nsec += (long)(task->timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&st->lock);
    bool cancelled = false;
    while (st->generation == task->generation) {
        int rc = pthread_cond_timedwait(&st->cond, &st->lock, &ts);
        if (rc == ETIMEDOUT)
            break;
    }
    if (st->generation != task->generation)
        cancelled = true;
    pthread_mutex_unlock(&st->lock);

    if (!cancelled && task->callback != NULL) {
        if (g_cb_class == NULL) {
            g_cb_class = AIBinder_Class_define("android.hardware.vibrator.IVibratorCallback",
                                               cb_on_create, cb_on_destroy, cb_on_transact);
        }
        if (g_cb_class != NULL)
            AIBinder_associateClass(task->callback, g_cb_class);
        AParcel* in = NULL;
        AParcel* out = NULL;
        if (AIBinder_prepareTransaction(task->callback, &in) == STATUS_OK) {
            AIBinder_transact(task->callback, CB_TX_ON_COMPLETE, &in, &out, FLAG_ONEWAY);
            if (out != NULL)
                AParcel_delete(out);
        }
    }

    if (task->callback != NULL)
        AIBinder_decStrong(task->callback);
    free(task);
    return NULL;
}

static int vibrator_on_locked(VibratorState* st, int32_t timeout_ms, float amp, AIBinder* callback) {
    if (ensure_open_locked(st) < 0)
        return -1;

    if (timeout_ms <= 0)
        return vibrator_off_locked(st);

    uint16_t len = (timeout_ms > 32767) ? 32767 : (uint16_t)timeout_ms;
    uint16_t mag = (uint16_t)(amp * 0xFFFF);
    if (mag == 0 && amp > 0.0f)
        mag = 1;

    struct ff_effect effect;
    memset(&effect, 0, sizeof(effect));
    effect.type = FF_RUMBLE;
    effect.id = (int16_t)st->effect_id;
    effect.u.rumble.strong_magnitude = mag;
    effect.u.rumble.weak_magnitude = mag;
    effect.replay.length = len;
    effect.replay.delay = 0;

    if (ioctl(st->fd, EVIOCSFF, &effect) < 0) {
        ALOGE("EVIOCSFF failed (id=%d, len=%u, mag=%u): %s",
              st->effect_id, len, mag, strerror(errno));
        /* Try resetting effect_id once if stale */
        if (st->effect_id >= 0) {
            st->effect_id = -1;
            effect.id = -1;
            if (ioctl(st->fd, EVIOCSFF, &effect) < 0) {
                ALOGE("EVIOCSFF retry failed: %s", strerror(errno));
                return -1;
            }
        } else {
            return -1;
        }
    }
    st->effect_id = effect.id;

    struct input_event play;
    memset(&play, 0, sizeof(play));
    play.type = EV_FF;
    play.code = (uint16_t)st->effect_id;
    play.value = 1;

    if (write(st->fd, &play, sizeof(play)) < 0) {
        ALOGE("EV_FF start failed: %s", strerror(errno));
        return -1;
    }

    st->generation++;
    pthread_cond_broadcast(&st->cond);

    if (callback != NULL) {
        CallbackTask* task = (CallbackTask*)calloc(1, sizeof(CallbackTask));
        if (task != NULL) {
            task->state = st;
            task->callback = callback;
            task->timeout_ms = timeout_ms;
            task->generation = st->generation;
            AIBinder_incStrong(callback);

            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&tid, &attr, callback_thread_fn, task) != 0) {
                AIBinder_decStrong(callback);
                free(task);
            }
            pthread_attr_destroy(&attr);
        }
    }

    return 0;
}

static binder_status_t write_ok(AParcel* out) {
    AStatus* st = AStatus_newOk();
    binder_status_t rc = AParcel_writeStatusHeader(out, st);
    AStatus_delete(st);
    return rc;
}

static binder_status_t write_exception(AParcel* out, binder_exception_t code) {
    AStatus* st = AStatus_fromExceptionCode(code);
    binder_status_t rc = AParcel_writeStatusHeader(out, st);
    AStatus_delete(st);
    return rc;
}

static void* on_create(void* args) {
    return args;
}

static void on_destroy(void* userData) {
    (void)userData;
}

static binder_status_t on_transact(AIBinder* binder,
                                   transaction_code_t code,
                                   const AParcel* in,
                                   AParcel* out) {
    VibratorState* st = (VibratorState*)AIBinder_getUserData(binder);
    binder_status_t rc;

    switch (code) {
        case TX_GET_CAPABILITIES: {
            rc = write_ok(out);
            if (rc != STATUS_OK) return rc;
            return AParcel_writeInt32(out, CAP_ON_CALLBACK | CAP_PERFORM_CALLBACK | CAP_AMPLITUDE_CONTROL);
        }

        case TX_OFF: {
            pthread_mutex_lock(&st->lock);
            vibrator_off_locked(st);
            pthread_mutex_unlock(&st->lock);
            return write_ok(out);
        }

        case TX_ON: {
            int32_t timeout_ms = 0;
            AIBinder* cb = NULL;
            rc = AParcel_readInt32(in, &timeout_ms);
            if (rc != STATUS_OK) return rc;
            rc = AParcel_readStrongBinder(in, &cb);
            if (rc != STATUS_OK) return rc;

            pthread_mutex_lock(&st->lock);
            int err = vibrator_on_locked(st, timeout_ms, st->amplitude, cb);
            pthread_mutex_unlock(&st->lock);

            if (cb != NULL)
                AIBinder_decStrong(cb);

            if (err < 0)
                return write_exception(out, EX_SERVICE_SPECIFIC);
            return write_ok(out);
        }

        case TX_PERFORM: {
            int32_t effect = 0;
            int8_t strength = 1;
            AIBinder* cb = NULL;
            rc = AParcel_readInt32(in, &effect);
            if (rc != STATUS_OK) return rc;
            rc = AParcel_readByte(in, &strength);
            if (rc != STATUS_OK) return rc;
            rc = AParcel_readStrongBinder(in, &cb);
            if (rc != STATUS_OK) return rc;

            int32_t dur_ms = 0;
            switch (effect) {
                case EFFECT_CLICK:        dur_ms = 20; break;
                case EFFECT_DOUBLE_CLICK: dur_ms = 40; break;
                case EFFECT_TICK:         dur_ms = 15; break;
                case EFFECT_THUD:         dur_ms = 30; break;
                case EFFECT_POP:          dur_ms = 20; break;
                case EFFECT_HEAVY_CLICK:  dur_ms = 35; break;
                case EFFECT_TEXTURE_TICK: dur_ms = 10; break;
                default:
                    if (cb != NULL) AIBinder_decStrong(cb);
                    return write_exception(out, EX_UNSUPPORTED_OPERATION);
            }

            float amp = 0.75f;
            if (strength == 0) amp = 0.45f;      /* LIGHT */
            else if (strength == 1) amp = 0.75f; /* MEDIUM */
            else if (strength == 2) amp = 1.0f;  /* STRONG */

            pthread_mutex_lock(&st->lock);
            int err = vibrator_on_locked(st, dur_ms, amp, cb);
            pthread_mutex_unlock(&st->lock);

            if (cb != NULL)
                AIBinder_decStrong(cb);

            if (err < 0)
                return write_exception(out, EX_SERVICE_SPECIFIC);
            rc = write_ok(out);
            if (rc != STATUS_OK) return rc;
            return AParcel_writeInt32(out, dur_ms);
        }

        case TX_GET_SUPPORTED_EFFECTS: {
            rc = write_ok(out);
            if (rc != STATUS_OK) return rc;
            return AParcel_writeInt32Array(out, kSupportedEffects,
                                           (int32_t)(sizeof(kSupportedEffects) / sizeof(kSupportedEffects[0])));
        }

        case TX_SET_AMPLITUDE: {
            float amp = 1.0f;
            rc = AParcel_readFloat(in, &amp);
            if (rc != STATUS_OK) return rc;
            if (!(amp > 0.0f && amp <= 1.0f)) {
                return write_exception(out, EX_ILLEGAL_ARGUMENT);
            }
            pthread_mutex_lock(&st->lock);
            st->amplitude = amp;
            if (ensure_open_locked(st) == 0)
                send_ff_gain_locked(st, amp);
            pthread_mutex_unlock(&st->lock);
            return write_ok(out);
        }

        case TX_GET_COMPOSITION_DELAY_MAX:
        case TX_GET_COMPOSITION_SIZE_MAX: {
            rc = write_ok(out);
            if (rc != STATUS_OK) return rc;
            return AParcel_writeInt32(out, 0);
        }

        case TX_GET_SUPPORTED_PRIMITIVES:
        case TX_GET_SUPPORTED_ALWAYS_ON:
        case TX_GET_SUPPORTED_BRAKING: {
            rc = write_ok(out);
            if (rc != STATUS_OK) return rc;
            int32_t empty = 0;
            return AParcel_writeInt32Array(out, &empty, 0);
        }

        case TX_GET_INTERFACE_VERSION: {
            rc = write_ok(out);
            if (rc != STATUS_OK) return rc;
            return AParcel_writeInt32(out, VIBRATOR_VERSION);
        }

        case TX_GET_INTERFACE_HASH: {
            rc = write_ok(out);
            if (rc != STATUS_OK) return rc;
            return AParcel_writeString(out, VIBRATOR_HASH, (int32_t)strlen(VIBRATOR_HASH));
        }

        case TX_SET_EXTERNAL_CONTROL:
        case TX_GET_PRIMITIVE_DURATION:
        case TX_COMPOSE:
        case TX_ALWAYS_ON_ENABLE:
        case TX_ALWAYS_ON_DISABLE:
        case TX_GET_RESONANT_FREQUENCY:
        case TX_GET_Q_FACTOR:
        case TX_GET_FREQUENCY_RESOLUTION:
        case TX_GET_FREQUENCY_MINIMUM:
        case TX_GET_BANDWIDTH_AMPLITUDE_MAP:
        case TX_GET_PWLE_PRIMITIVE_DUR_MAX:
        case TX_GET_PWLE_COMPOSITION_SIZE_MAX:
        case TX_COMPOSE_PWLE:
            return write_exception(out, EX_UNSUPPORTED_OPERATION);

        default:
            return STATUS_UNKNOWN_TRANSACTION;
    }
}

int main(void) {
    ABinderProcess_setThreadPoolMaxThreadCount(0);

    VibratorState state;
    memset(&state, 0, sizeof(state));
    pthread_mutex_init(&state.lock, NULL);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&state.cond, &cattr);
    pthread_condattr_destroy(&cattr);

    state.fd = open_haptic_device();
    state.effect_id = -1;
    state.amplitude = 1.0f;

    AIBinder_Class* clazz = AIBinder_Class_define(VIBRATOR_DESCRIPTOR, on_create, on_destroy, on_transact);
    if (clazz == NULL) {
        ALOGE("Failed to define AIBinder_Class for %s", VIBRATOR_DESCRIPTOR);
        return 1;
    }

    AIBinder* binder = AIBinder_new(clazz, &state);
    if (binder == NULL) {
        ALOGE("Failed to create AIBinder");
        return 1;
    }

    binder_status_t status = AServiceManager_addService(binder, VIBRATOR_INSTANCE);
    if (status != STATUS_OK) {
        ALOGE("AServiceManager_addService(%s) failed: %d", VIBRATOR_INSTANCE, status);
        AIBinder_decStrong(binder);
        return 1;
    }

    ALOGI("Registered %s (mainline FF_RUMBLE, fd=%d)", VIBRATOR_INSTANCE, state.fd);
    ABinderProcess_joinThreadPool();

    AIBinder_decStrong(binder);
    return 0;
}
