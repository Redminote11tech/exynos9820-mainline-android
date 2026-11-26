/*
 * Standalone Android AIDL v3 ISensors HAL for Exynos 9820 / Galaxy S10+ (beyond2lte)
 * Reads live Mainline Linux 7.2 BCM4775 IIO sensor hub devices:
 *   - bcm4775-accel   -> SENSOR_TYPE_ACCELEROMETER (1)
 *   - bcm4775-light   -> SENSOR_TYPE_LIGHT (5)
 *   - bcm4775-prox    -> SENSOR_TYPE_PROXIMITY (8)
 *   - bcm4775-compass -> SENSOR_TYPE_HEADING (42)
 *
 * Links only against standard LLNDK/Bionic libraries: libbinder_ndk.so, liblog.so, libc.so, libm.so.
 */

#include <atomic>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <android/log.h>

#define LOG_TAG "sensors-mainline"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Platform/LLNDK symbols from libbinder_ndk.so */
extern "C" {
binder_status_t AServiceManager_addService(AIBinder* binder, const char* instance);
void ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_startThreadPool(void);
void ABinderProcess_joinThreadPool(void);
void AIBinder_markVintfStability(AIBinder* binder);
}

/* android.hardware.sensors.ISensors v3 constants */
static constexpr const char* kSensorsDescriptor = "android.hardware.sensors.ISensors";
static constexpr const char* kSensorsInstance   = "android.hardware.sensors.ISensors/default";
static constexpr int32_t     kSensorsVersion    = 3;
static constexpr const char* kSensorsHash       = "b051523be857524c99057b69f253a7d0bd295400";

/* ISensors v3 transaction codes (FIRST_CALL_TRANSACTION = 1) */
enum : transaction_code_t {
    TX_ACTIVATE                  = FIRST_CALL_TRANSACTION + 0,  /* 1 */
    TX_BATCH                     = FIRST_CALL_TRANSACTION + 1,  /* 2 */
    TX_CONFIG_DIRECT_REPORT      = FIRST_CALL_TRANSACTION + 2,  /* 3 */
    TX_FLUSH                     = FIRST_CALL_TRANSACTION + 3,  /* 4 */
    TX_GET_SENSORS_LIST          = FIRST_CALL_TRANSACTION + 4,  /* 5 */
    TX_INITIALIZE                = FIRST_CALL_TRANSACTION + 5,  /* 6 */
    TX_INJECT_SENSOR_DATA        = FIRST_CALL_TRANSACTION + 6,  /* 7 */
    TX_REGISTER_DIRECT_CHANNEL   = FIRST_CALL_TRANSACTION + 7,  /* 8 */
    TX_SET_OPERATION_MODE        = FIRST_CALL_TRANSACTION + 8,  /* 9 */
    TX_UNREGISTER_DIRECT_CHANNEL = FIRST_CALL_TRANSACTION + 9,  /* 10 */
    TX_GET_INTERFACE_HASH        = FIRST_CALL_TRANSACTION + 16777213, /* 0x00fffffe */
    TX_GET_INTERFACE_VERSION     = FIRST_CALL_TRANSACTION + 16777214, /* 0x00ffffff */
};

/* ISensors error / flag constants */
static constexpr int32_t  kErrBadValue                    = -22;
static constexpr uint32_t kEventQueueFlagReadAndProcess   = (1u << 0);

/* SensorType values */
enum SensorType : int32_t {
    SENSOR_TYPE_META_DATA     = 0,
    SENSOR_TYPE_ACCELEROMETER = 1,
    SENSOR_TYPE_LIGHT         = 5,
    SENSOR_TYPE_PROXIMITY     = 8,
    SENSOR_TYPE_HEADING       = 42,
};

/* SensorStatus values */
enum SensorStatus : int8_t {
    SENSOR_STATUS_UNRELIABLE      = 0,
    SENSOR_STATUS_ACCURACY_LOW    = 1,
    SENSOR_STATUS_ACCURACY_MEDIUM = 2,
    SENSOR_STATUS_ACCURACY_HIGH   = 3,
};

/* SensorInfo flag bits */
static constexpr int32_t kFlagWakeUp         = 1;
static constexpr int32_t kFlagContinuousMode = 0;
static constexpr int32_t kFlagOnChangeMode   = 2;

/*
 * Exact binary layout of @FixedSize aidl::android::hardware::sensors::Event (v3).
 * Verified against /vendor/lib64/android.hardware.sensors-V3-ndk.so and
 * /vendor/bin/hw/android.hardware.sensors-service.samsung-multihal (quantum = 0x60 = 96 bytes).
 */
enum EventPayloadTag : int8_t {
    TAG_VEC3     = 0,
    TAG_META     = 3,
    TAG_SCALAR   = 4,
    TAG_HEADING  = 14,
};

struct AidlVec3 {
    float  x;
    float  y;
    float  z;
    int8_t status;
    uint8_t pad[3];
};

struct AidlMetaData {
    int32_t what; /* 1 = META_DATA_FLUSH_COMPLETE */
};

struct AidlHeading {
    float heading;
    float accuracy;
};

struct alignas(8) AidlEventPayload {
    int8_t  tag;
    uint8_t pad[7];
    union {
        AidlVec3     vec3;
        AidlMetaData meta;
        float        scalar;
        int64_t      stepCount;
        AidlHeading  heading;
        uint8_t      raw[72]; /* AdditionalInfo (68 B) rounded to 8-byte alignment = 72 B */
    } u;
};

struct alignas(8) AidlEvent {
    int64_t          timestamp;    /* offset 0x00, CLOCK_BOOTTIME ns */
    int32_t          sensorHandle; /* offset 0x08 */
    int32_t          sensorType;   /* offset 0x0c */
    AidlEventPayload payload;      /* offset 0x10, size 0x50 (80 B) */
};

static_assert(sizeof(AidlEvent) == 96, "AidlEvent must be 96 bytes (0x60)");
static_assert(offsetof(AidlEvent, sensorHandle) == 0x08, "sensorHandle offset mismatch");
static_assert(offsetof(AidlEvent, sensorType) == 0x0c, "sensorType offset mismatch");
static_assert(offsetof(AidlEvent, payload) == 0x10, "payload offset mismatch");
static_assert(offsetof(AidlEventPayload, u) == 0x08, "EventPayload union offset mismatch");

/* =========================================================================
 * Time helpers
 * ========================================================================= */

static int64_t boottime_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int64_t monotonic_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* =========================================================================
 * FMQ (Fast Message Queue) descriptor parsing & mmap ring buffer
 * ========================================================================= */

enum GrantorPos : size_t {
    GRANTOR_READ_PTR   = 0,
    GRANTOR_WRITE_PTR  = 1,
    GRANTOR_DATA_PTR   = 2,
    GRANTOR_EVFLAG_PTR = 3,
};

struct GrantorDesc {
    int32_t fdIndex = 0;
    int32_t offset  = 0;
    int64_t extent  = 0;
};

struct MqDesc {
    std::vector<GrantorDesc> grantors;
    std::vector<int>         fds;
    std::vector<int32_t>     ints;
    int32_t                  quantum = 0;
    int32_t                  flags   = 0;

    void closeFds() {
        for (int fd : fds) {
            if (fd >= 0) close(fd);
        }
        fds.clear();
    }
    ~MqDesc() { closeFds(); }
};

struct MappedRegion {
    void*  base = nullptr;
    size_t len  = 0;
};

struct FmqQueue {
    std::atomic<uint64_t>* readPtr    = nullptr;
    std::atomic<uint64_t>* writePtr   = nullptr;
    uint8_t*               ring       = nullptr;
    size_t                 ringSize   = 0;
    std::atomic<uint32_t>* evFlagWord = nullptr;
    size_t                 quantum    = 0;
    std::vector<MappedRegion> regions;

    void reset() {
        for (const auto& r : regions) {
            if (r.base && r.base != MAP_FAILED) {
                munmap(r.base, r.len);
            }
        }
        regions.clear();
        readPtr = nullptr;
        writePtr = nullptr;
        ring = nullptr;
        ringSize = 0;
        evFlagWord = nullptr;
        quantum = 0;
    }

    ~FmqQueue() { reset(); }

    void* mapGrantor(const MqDesc& desc, size_t idx) {
        if (idx >= desc.grantors.size()) return nullptr;
        const GrantorDesc& g = desc.grantors[idx];
        if (g.fdIndex < 0 || (size_t)g.fdIndex >= desc.fds.size()) return nullptr;
        int fd = desc.fds[(size_t)g.fdIndex];
        if (fd < 0 || g.extent <= 0 || g.offset < 0) return nullptr;

        size_t pageSize = (size_t)sysconf(_SC_PAGESIZE);
        size_t off = (size_t)g.offset;
        size_t pageOff = off & ~(pageSize - 1);
        size_t delta = off - pageOff;
        size_t mapLen = delta + (size_t)g.extent;

        void* base = mmap(nullptr, mapLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)pageOff);
        if (base == MAP_FAILED) {
            ALOGE("FMQ mmap grantor[%zu] failed (fd=%d off=%zu len=%zu): %s",
                  idx, fd, off, mapLen, strerror(errno));
            return nullptr;
        }
        regions.push_back({base, mapLen});
        return (uint8_t*)base + delta;
    }

    bool init(const MqDesc& desc, bool requireEventFlag) {
        reset();
        if (desc.grantors.size() < 3 || desc.quantum <= 0) {
            ALOGE("FMQ invalid descriptor: grantors=%zu quantum=%d",
                  desc.grantors.size(), desc.quantum);
            return false;
        }
        readPtr  = reinterpret_cast<std::atomic<uint64_t>*>(mapGrantor(desc, GRANTOR_READ_PTR));
        writePtr = reinterpret_cast<std::atomic<uint64_t>*>(mapGrantor(desc, GRANTOR_WRITE_PTR));
        ring     = reinterpret_cast<uint8_t*>(mapGrantor(desc, GRANTOR_DATA_PTR));
        if (!readPtr || !writePtr || !ring) {
            reset();
            return false;
        }
        ringSize = (size_t)desc.grantors[GRANTOR_DATA_PTR].extent;
        quantum  = (size_t)desc.quantum;

        if (desc.grantors.size() > GRANTOR_EVFLAG_PTR) {
            evFlagWord = reinterpret_cast<std::atomic<uint32_t>*>(mapGrantor(desc, GRANTOR_EVFLAG_PTR));
        }
        if (requireEventFlag && !evFlagWord) {
            ALOGE("FMQ missing EventFlag grantor");
            reset();
            return false;
        }
        return true;
    }

    bool isValid() const {
        return readPtr != nullptr && writePtr != nullptr && ring != nullptr &&
               ringSize >= quantum && quantum > 0;
    }

    bool writeEvent(const AidlEvent& ev) {
        if (!isValid() || quantum < sizeof(AidlEvent)) return false;

        uint64_t wp = writePtr->load(std::memory_order_acquire);
        uint64_t rp = readPtr->load(std::memory_order_acquire);
        if (wp < rp) return false;
        uint64_t used = wp - rp;
        if (used > ringSize || (ringSize - used) < quantum) {
            return false; /* Queue full */
        }

        size_t writeOffset = (size_t)(wp % ringSize);
        if (writeOffset + quantum <= ringSize) {
            memcpy(ring + writeOffset, &ev, sizeof(AidlEvent));
            if (quantum > sizeof(AidlEvent)) {
                memset(ring + writeOffset + sizeof(AidlEvent), 0, quantum - sizeof(AidlEvent));
            }
        } else {
            /* Wrap-around copy */
            uint8_t tmp[128];
            size_t copyLen = quantum <= sizeof(tmp) ? quantum : sizeof(AidlEvent);
            memset(tmp, 0, copyLen);
            memcpy(tmp, &ev, sizeof(AidlEvent));
            size_t first = ringSize - writeOffset;
            memcpy(ring + writeOffset, tmp, first);
            memcpy(ring, tmp + first, copyLen - first);
        }

        writePtr->store(wp + quantum, std::memory_order_release);

        if (evFlagWord) {
            uint32_t old = evFlagWord->fetch_or(kEventQueueFlagReadAndProcess, std::memory_order_acq_rel);
            if ((~old & kEventQueueFlagReadAndProcess) != 0) {
                syscall(SYS_futex, evFlagWord, FUTEX_WAKE_BITSET, INT_MAX,
                        nullptr, nullptr, kEventQueueFlagReadAndProcess);
            }
        }
        return true;
    }

    /* Drain wakeLockQueue so sensorservice never blocks on wake-up sensor accounting */
    void drainAll() {
        if (!readPtr || !writePtr) return;
        uint64_t wp = writePtr->load(std::memory_order_acquire);
        uint64_t rp = readPtr->load(std::memory_order_acquire);
        if (wp > rp) {
            readPtr->store(wp, std::memory_order_release);
        }
    }
};

/* =========================================================================
 * AIDL Parcel read/write helpers
 * ========================================================================= */

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

static binder_status_t write_service_specific(AParcel* out, int32_t err) {
    AStatus* st = AStatus_fromServiceSpecificError(err);
    binder_status_t rc = AParcel_writeStatusHeader(out, st);
    AStatus_delete(st);
    return rc;
}

static bool grantor_array_allocator(void* arrayData, int32_t length) {
    if (length < 0) return false;
    auto* vec = static_cast<std::vector<GrantorDesc>*>(arrayData);
    vec->resize((size_t)length);
    return true;
}

static binder_status_t read_grantor_element(const AParcel* in, void* arrayData, size_t index) {
    auto* vec = static_cast<std::vector<GrantorDesc>*>(arrayData);
    int32_t nonNull = 0;
    binder_status_t rc = AParcel_readInt32(in, &nonNull);
    if (rc != STATUS_OK) return rc;
    if (nonNull == 0) return STATUS_OK;

    int32_t startPos = AParcel_getDataPosition(in);
    int32_t parcelSize = 0;
    rc = AParcel_readInt32(in, &parcelSize);
    if (rc != STATUS_OK) return rc;
    if (parcelSize < 4) return STATUS_BAD_VALUE;

    GrantorDesc& g = (*vec)[index];
    if (AParcel_getDataPosition(in) - startPos < parcelSize) {
        rc = AParcel_readInt32(in, &g.fdIndex);
        if (rc != STATUS_OK) return rc;
    }
    if (AParcel_getDataPosition(in) - startPos < parcelSize) {
        rc = AParcel_readInt32(in, &g.offset);
        if (rc != STATUS_OK) return rc;
    }
    if (AParcel_getDataPosition(in) - startPos < parcelSize) {
        rc = AParcel_readInt64(in, &g.extent);
        if (rc != STATUS_OK) return rc;
    }
    return AParcel_setDataPosition(in, startPos + parcelSize);
}

static bool fd_array_allocator(void* arrayData, int32_t length) {
    if (length < 0) return false;
    auto* vec = static_cast<std::vector<int>*>(arrayData);
    vec->assign((size_t)length, -1);
    return true;
}

static binder_status_t read_fd_element(const AParcel* in, void* arrayData, size_t index) {
    auto* vec = static_cast<std::vector<int>*>(arrayData);
    int fd = -1;
    binder_status_t rc = AParcel_readParcelFileDescriptor(in, &fd);
    if (rc != STATUS_OK) return rc;
    (*vec)[index] = fd;
    return STATUS_OK;
}

static bool int32_array_allocator(void* arrayData, int32_t length, int32_t** outBuffer) {
    if (length < 0) return false;
    auto* vec = static_cast<std::vector<int32_t>*>(arrayData);
    vec->resize((size_t)length);
    *outBuffer = vec->data();
    return true;
}

static binder_status_t read_native_handle(const AParcel* in, MqDesc* desc) {
    int32_t nonNull = 0;
    binder_status_t rc = AParcel_readInt32(in, &nonNull);
    if (rc != STATUS_OK) return rc;
    if (nonNull == 0) return STATUS_OK;

    int32_t startPos = AParcel_getDataPosition(in);
    int32_t parcelSize = 0;
    rc = AParcel_readInt32(in, &parcelSize);
    if (rc != STATUS_OK) return rc;
    if (parcelSize < 4) return STATUS_BAD_VALUE;

    if (AParcel_getDataPosition(in) - startPos < parcelSize) {
        rc = AParcel_readParcelableArray(in, &desc->fds, fd_array_allocator, read_fd_element);
        if (rc != STATUS_OK) return rc;
    }
    if (AParcel_getDataPosition(in) - startPos < parcelSize) {
        rc = AParcel_readInt32Array(in, &desc->ints, int32_array_allocator);
        if (rc != STATUS_OK) return rc;
    }
    return AParcel_setDataPosition(in, startPos + parcelSize);
}

static binder_status_t read_mq_descriptor(const AParcel* in, MqDesc* desc) {
    int32_t nonNull = 0;
    binder_status_t rc = AParcel_readInt32(in, &nonNull);
    if (rc != STATUS_OK) return rc;
    if (nonNull == 0) return STATUS_BAD_VALUE;

    int32_t startPos = AParcel_getDataPosition(in);
    int32_t parcelSize = 0;
    rc = AParcel_readInt32(in, &parcelSize);
    if (rc != STATUS_OK) return rc;
    if (parcelSize < 4) return STATUS_BAD_VALUE;

    if (AParcel_getDataPosition(in) - startPos < parcelSize) {
        rc = AParcel_readParcelableArray(in, &desc->grantors, grantor_array_allocator, read_grantor_element);
        if (rc != STATUS_OK) return rc;
    }
    if (AParcel_getDataPosition(in) - startPos < parcelSize) {
        rc = read_native_handle(in, desc);
        if (rc != STATUS_OK) return rc;
    }
    if (AParcel_getDataPosition(in) - startPos < parcelSize) {
        rc = AParcel_readInt32(in, &desc->quantum);
        if (rc != STATUS_OK) return rc;
    }
    if (AParcel_getDataPosition(in) - startPos < parcelSize) {
        rc = AParcel_readInt32(in, &desc->flags);
        if (rc != STATUS_OK) return rc;
    }
    return AParcel_setDataPosition(in, startPos + parcelSize);
}

/* =========================================================================
 * IIO sysfs helpers
 * ========================================================================= */

static bool read_sysfs_str(const char* path, char* buf, size_t len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
        buf[--n] = '\0';
    }
    return true;
}

static bool read_sysfs_int(const char* dir, const char* attr, int64_t* out) {
    char path[256];
    char buf[64];
    snprintf(path, sizeof(path), "%s/%s", dir, attr);
    if (!read_sysfs_str(path, buf, sizeof(buf))) return false;
    char* end = nullptr;
    long long v = strtoll(buf, &end, 10);
    if (end == buf) return false;
    *out = (int64_t)v;
    return true;
}

static bool read_sysfs_double(const char* dir, const char* attr, double* out) {
    char path[256];
    char buf[64];
    snprintf(path, sizeof(path), "%s/%s", dir, attr);
    if (!read_sysfs_str(path, buf, sizeof(buf))) return false;
    char* end = nullptr;
    double v = strtod(buf, &end);
    if (end == buf) return false;
    *out = v;
    return true;
}

static void write_sysfs_int(const char* dir, const char* attr, int value) {
    char path[256];
    char buf[32];
    snprintf(path, sizeof(path), "%s/%s", dir, attr);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    int len = snprintf(buf, sizeof(buf), "%d", value);
    if (len > 0) {
        (void)write(fd, buf, (size_t)len);
    }
    close(fd);
}

/* =========================================================================
 * Sensor definitions & state
 * ========================================================================= */

enum SensorHandle : int32_t {
    HANDLE_ACCEL   = 1,
    HANDLE_LIGHT   = 2,
    HANDLE_PROX    = 3,
    HANDLE_COMPASS = 4,
};

struct SensorInfoData {
    int32_t     sensorHandle;
    const char* name;
    const char* vendor;
    int32_t     version;
    int32_t     type;
    const char* typeAsString;
    float       maxRange;
    float       resolution;
    float       power;
    int32_t     minDelayUs;
    int32_t     fifoReservedEventCount;
    int32_t     fifoMaxEventCount;
    const char* requiredPermission;
    int32_t     maxDelayUs;
    int32_t     flags;
};

struct SensorEntry {
    SensorInfoData info;
    const char*    iioName;
    char           iioPath[128];
    bool           present;
    bool           enabled;
    bool           onChange;
    bool           wakeUp;
    bool           hasLastValue;
    float          lastValue;
    double         scale;
    int64_t        periodNs;
    int64_t        nextPollNs;
};

struct SensorsState {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    pthread_mutex_t fmqLock;
    pthread_t       pollThread;
    bool            pollStarted;
    bool            stop;

    FmqQueue        eventQueue;
    FmqQueue        wakeLockQueue;
    AIBinder*       callback;

    SensorEntry     sensors[4];
};

static void discover_iio_devices_locked(SensorsState* st) {
    DIR* dir = opendir("/sys/bus/iio/devices");
    if (!dir) return;

    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        if (strncmp(de->d_name, "iio:device", 10) != 0) continue;
        char devPath[128];
        char namePath[160];
        char name[64];
        snprintf(devPath, sizeof(devPath), "/sys/bus/iio/devices/%s", de->d_name);
        snprintf(namePath, sizeof(namePath), "%s/name", devPath);
        if (!read_sysfs_str(namePath, name, sizeof(name))) continue;

        for (auto& s : st->sensors) {
            if (strcmp(name, s.iioName) == 0) {
                bool wasPresent = s.present;
                snprintf(s.iioPath, sizeof(s.iioPath), "%s", devPath);
                s.present = true;

                if (s.info.sensorHandle == HANDLE_ACCEL) {
                    double sc = 0.0;
                    if (read_sysfs_double(devPath, "in_accel_scale", &sc) && sc > 0.0) {
                        s.scale = sc;
                        s.info.resolution = (float)sc;
                    }
                } else if (s.info.sensorHandle == HANDLE_COMPASS) {
                    double sc = 0.0;
                    if (read_sysfs_double(devPath, "in_rot_scale", &sc) && sc > 0.0) {
                        s.scale = sc;
                        s.info.resolution = (float)sc;
                    }
                }
                if (!wasPresent) {
                    ALOGI("Discovered %s at %s (scale=%.9f)", name, devPath, s.scale);
                }
            }
        }
    }
    closedir(dir);
}

static SensorEntry* find_sensor_locked(SensorsState* st, int32_t handle) {
    for (auto& s : st->sensors) {
        if (s.info.sensorHandle == handle) return &s;
    }
    return nullptr;
}

static bool post_event(SensorsState* st, const AidlEvent& ev, bool isWakeUp) {
    pthread_mutex_lock(&st->fmqLock);
    if (isWakeUp) {
        st->wakeLockQueue.drainAll();
    }
    bool ok = st->eventQueue.writeEvent(ev);
    pthread_mutex_unlock(&st->fmqLock);
    return ok;
}

static bool read_sensor_sample(const SensorEntry& s, AidlEvent* ev, float* outScalar) {
    if (!s.present || s.iioPath[0] == '\0') return false;

    memset(ev, 0, sizeof(*ev));
    ev->sensorHandle = s.info.sensorHandle;
    ev->sensorType   = s.info.type;

    switch (s.info.sensorHandle) {
        case HANDLE_ACCEL: {
            int64_t rx = 0, ry = 0, rz = 0;
            if (!read_sysfs_int(s.iioPath, "in_accel_x_raw", &rx) ||
                !read_sysfs_int(s.iioPath, "in_accel_y_raw", &ry) ||
                !read_sysfs_int(s.iioPath, "in_accel_z_raw", &rz)) {
                return false;
            }
            ev->timestamp = boottime_ns();
            ev->payload.tag = TAG_VEC3;
            /* Beyond2lte (S10+) LSM6DSO PCB is rotated 90 deg CW around +Z vs Android portrait:
             * raw_x = -9.8 m/s^2 when upright portrait -> android_x = +ry, android_y = -rx, android_z = +rz */
            ev->payload.u.vec3.x      = (float)((double)ry * s.scale);
            ev->payload.u.vec3.y      = (float)(-(double)rx * s.scale);
            ev->payload.u.vec3.z      = (float)((double)rz * s.scale);
            ev->payload.u.vec3.status = SENSOR_STATUS_ACCURACY_HIGH;
            return true;
        }

        case HANDLE_LIGHT: {
            int64_t lux = 0;
            if (!read_sysfs_int(s.iioPath, "in_illuminance_input", &lux)) {
                return false;
            }
            if (lux < 0) lux = 0;
            float val = (float)lux;
            ev->timestamp = boottime_ns();
            ev->payload.tag = TAG_SCALAR;
            ev->payload.u.scalar = val;
            *outScalar = val;
            return true;
        }

        case HANDLE_PROX: {
            int64_t raw = 0;
            if (!read_sysfs_int(s.iioPath, "in_proximity_raw", &raw)) {
                return false;
            }
            /* bcm4775-shub.c: 1 = near -> 0.0 cm, 0 = far -> 5.0 cm */
            float distCm = (raw != 0) ? 0.0f : 5.0f;
            ev->timestamp = boottime_ns();
            ev->payload.tag = TAG_SCALAR;
            ev->payload.u.scalar = distCm;
            *outScalar = distCm;
            return true;
        }

        case HANDLE_COMPASS: {
            int64_t raw = 0;
            if (!read_sysfs_int(s.iioPath, "in_rot_from_north_magnetic_tilt_comp_raw", &raw)) {
                return false;
            }
            float deg = (float)((double)raw * s.scale);
            if (deg < 0.0f) deg += 360.0f;
            if (deg >= 360.0f) deg = fmodf(deg, 360.0f);
            ev->timestamp = boottime_ns();
            ev->payload.tag = TAG_HEADING;
            ev->payload.u.heading.heading  = deg;
            ev->payload.u.heading.accuracy = 5.0f;
            return true;
        }

        default:
            return false;
    }
}

static void* poll_thread_fn(void* arg) {
    auto* st = static_cast<SensorsState*>(arg);
    ALOGI("Sensor poll thread started");

    while (true) {
        SensorEntry due[4];
        int dueCount = 0;

        pthread_mutex_lock(&st->lock);
        while (!st->stop) {
            int64_t now = monotonic_ns();
            int64_t earliest = INT64_MAX;
            dueCount = 0;

            for (auto& s : st->sensors) {
                if (!s.enabled || !s.present) continue;
                if (s.nextPollNs <= now) {
                    due[dueCount++] = s;
                    s.nextPollNs = now + s.periodNs;
                }
                if (s.nextPollNs < earliest) {
                    earliest = s.nextPollNs;
                }
            }

            if (dueCount > 0) break;

            if (earliest == INT64_MAX) {
                pthread_cond_wait(&st->cond, &st->lock);
            } else {
                struct timespec ts;
                ts.tv_sec  = (time_t)(earliest / 1000000000LL);
                ts.tv_nsec = (long)(earliest % 1000000000LL);
                pthread_cond_timedwait(&st->cond, &st->lock, &ts);
            }
        }
        bool shouldStop = st->stop;
        pthread_mutex_unlock(&st->lock);

        if (shouldStop) break;

        for (int i = 0; i < dueCount; i++) {
            AidlEvent ev;
            float scalarVal = 0.0f;
            if (!read_sensor_sample(due[i], &ev, &scalarVal)) continue;

            bool emit = true;
            if (due[i].onChange) {
                pthread_mutex_lock(&st->lock);
                SensorEntry* live = find_sensor_locked(st, due[i].info.sensorHandle);
                if (live && live->enabled) {
                    if (live->hasLastValue && live->lastValue == scalarVal) {
                        emit = false;
                    } else {
                        live->hasLastValue = true;
                        live->lastValue = scalarVal;
                    }
                } else {
                    emit = false;
                }
                pthread_mutex_unlock(&st->lock);
            }

            if (emit) {
                post_event(st, ev, due[i].wakeUp);
            }
        }
    }

    ALOGI("Sensor poll thread exiting");
    return nullptr;
}

/* =========================================================================
 * SensorInfo Parcel serialization
 * ========================================================================= */

static binder_status_t write_sensor_info_element(AParcel* out, const void* arrayData, size_t index) {
    const auto* list = static_cast<const std::vector<SensorInfoData>*>(arrayData);
    const SensorInfoData& s = (*list)[index];

    binder_status_t rc = AParcel_writeInt32(out, 1); /* non-null indicator */
    if (rc != STATUS_OK) return rc;

    int32_t startPos = AParcel_getDataPosition(out);
    rc = AParcel_writeInt32(out, 0); /* placeholder for parcelable size */
    if (rc != STATUS_OK) return rc;

    if ((rc = AParcel_writeInt32(out, s.sensorHandle)) != STATUS_OK) return rc;
    if ((rc = AParcel_writeString(out, s.name, (int32_t)strlen(s.name))) != STATUS_OK) return rc;
    if ((rc = AParcel_writeString(out, s.vendor, (int32_t)strlen(s.vendor))) != STATUS_OK) return rc;
    if ((rc = AParcel_writeInt32(out, s.version)) != STATUS_OK) return rc;
    if ((rc = AParcel_writeInt32(out, s.type)) != STATUS_OK) return rc;
    if ((rc = AParcel_writeString(out, s.typeAsString, (int32_t)strlen(s.typeAsString))) != STATUS_OK) return rc;
    if ((rc = AParcel_writeFloat(out, s.maxRange)) != STATUS_OK) return rc;
    if ((rc = AParcel_writeFloat(out, s.resolution)) != STATUS_OK) return rc;
    if ((rc = AParcel_writeFloat(out, s.power)) != STATUS_OK) return rc;
    if ((rc = AParcel_writeInt32(out, s.minDelayUs)) != STATUS_OK) return rc;
    if ((rc = AParcel_writeInt32(out, s.fifoReservedEventCount)) != STATUS_OK) return rc;
    if ((rc = AParcel_writeInt32(out, s.fifoMaxEventCount)) != STATUS_OK) return rc;
    if ((rc = AParcel_writeString(out, s.requiredPermission, (int32_t)strlen(s.requiredPermission))) != STATUS_OK) return rc;
    if ((rc = AParcel_writeInt32(out, s.maxDelayUs)) != STATUS_OK) return rc;
    if ((rc = AParcel_writeInt32(out, s.flags)) != STATUS_OK) return rc;

    int32_t endPos = AParcel_getDataPosition(out);
    if ((rc = AParcel_setDataPosition(out, startPos)) != STATUS_OK) return rc;
    if ((rc = AParcel_writeInt32(out, endPos - startPos)) != STATUS_OK) return rc;
    return AParcel_setDataPosition(out, endPos);
}

/* =========================================================================
 * AIDL Binder callbacks
 * ========================================================================= */

static void* on_create(void* args) { return args; }
static void  on_destroy(void* userData) { (void)userData; }

static binder_status_t on_transact(AIBinder* binder,
                                   transaction_code_t code,
                                   const AParcel* in,
                                   AParcel* out) {
    auto* st = static_cast<SensorsState*>(AIBinder_getUserData(binder));
    binder_status_t rc;

    switch (code) {
        case TX_ACTIVATE: {
            int32_t handle = 0;
            bool enabled = false;
            if ((rc = AParcel_readInt32(in, &handle)) != STATUS_OK) return rc;
            if ((rc = AParcel_readBool(in, &enabled)) != STATUS_OK) return rc;

            pthread_mutex_lock(&st->lock);
            SensorEntry* s = find_sensor_locked(st, handle);
            if (!s || !s->present) {
                discover_iio_devices_locked(st);
                s = find_sensor_locked(st, handle);
            }
            if (!s || !s->present) {
                pthread_mutex_unlock(&st->lock);
                return write_service_specific(out, kErrBadValue);
            }
            s->enabled = enabled;
            if (enabled) {
                s->hasLastValue = false;
                s->nextPollNs = monotonic_ns(); /* Emit immediately */
            }
            pthread_cond_broadcast(&st->cond);
            pthread_mutex_unlock(&st->lock);

            ALOGI("activate(handle=%d [%s], enabled=%d)", handle, s->iioName, enabled ? 1 : 0);
            return write_ok(out);
        }

        case TX_BATCH: {
            int32_t handle = 0;
            int64_t samplingPeriodNs = 0;
            int64_t maxReportLatencyNs = 0;
            if ((rc = AParcel_readInt32(in, &handle)) != STATUS_OK) return rc;
            if ((rc = AParcel_readInt64(in, &samplingPeriodNs)) != STATUS_OK) return rc;
            if ((rc = AParcel_readInt64(in, &maxReportLatencyNs)) != STATUS_OK) return rc;
            (void)maxReportLatencyNs;

            if (samplingPeriodNs < 0 || maxReportLatencyNs < 0) {
                return write_service_specific(out, kErrBadValue);
            }

            pthread_mutex_lock(&st->lock);
            SensorEntry* s = find_sensor_locked(st, handle);
            if (!s || !s->present) {
                pthread_mutex_unlock(&st->lock);
                return write_service_specific(out, kErrBadValue);
            }

            if (s->onChange) {
                /* On-change sensors poll sysfs at 200ms (or faster if requested, clamped [100ms, 500ms]) */
                int64_t p = samplingPeriodNs;
                if (p <= 0) p = 200000000LL;
                if (p < 100000000LL) p = 100000000LL;
                if (p > 500000000LL) p = 500000000LL;
                s->periodNs = p;
            } else {
                int64_t minNs = (int64_t)s->info.minDelayUs * 1000LL;
                int64_t maxNs = (int64_t)s->info.maxDelayUs * 1000LL;
                int64_t p = samplingPeriodNs;
                if (p < minNs) p = minNs;
                if (maxNs > 0 && p > maxNs) p = maxNs;
                s->periodNs = p;

                if (handle == HANDLE_ACCEL && p > 0) {
                    int hz = (int)(1000000000LL / p);
                    if (hz < 1) hz = 1;
                    if (hz > 200) hz = 200;
                    write_sysfs_int(s->iioPath, "in_accel_sampling_frequency", hz);
                }
            }
            if (s->enabled) {
                s->nextPollNs = monotonic_ns();
                pthread_cond_broadcast(&st->cond);
            }
            pthread_mutex_unlock(&st->lock);
            return write_ok(out);
        }

        case TX_FLUSH: {
            int32_t handle = 0;
            if ((rc = AParcel_readInt32(in, &handle)) != STATUS_OK) return rc;

            pthread_mutex_lock(&st->lock);
            SensorEntry* s = find_sensor_locked(st, handle);
            bool valid = (s != nullptr && s->present);
            pthread_mutex_unlock(&st->lock);

            if (!valid) {
                return write_service_specific(out, kErrBadValue);
            }

            AidlEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.timestamp         = boottime_ns();
            ev.sensorHandle      = handle;
            ev.sensorType        = SENSOR_TYPE_META_DATA;
            ev.payload.tag       = TAG_META;
            ev.payload.u.meta.what = 1; /* META_DATA_FLUSH_COMPLETE */
            post_event(st, ev, false);
            return write_ok(out);
        }

        case TX_GET_SENSORS_LIST: {
            std::vector<SensorInfoData> list;
            pthread_mutex_lock(&st->lock);
            discover_iio_devices_locked(st);
            for (const auto& s : st->sensors) {
                if (s.present) {
                    list.push_back(s.info);
                }
            }
            pthread_mutex_unlock(&st->lock);

            ALOGI("getSensorsList() -> %zu sensors", list.size());
            if ((rc = write_ok(out)) != STATUS_OK) return rc;
            return AParcel_writeParcelableArray(out, &list, (int32_t)list.size(),
                                                write_sensor_info_element);
        }

        case TX_INITIALIZE: {
            MqDesc eventDesc;
            MqDesc wakeLockDesc;
            AIBinder* cb = nullptr;

            if ((rc = read_mq_descriptor(in, &eventDesc)) != STATUS_OK) {
                ALOGE("initialize: failed to read eventQueueDescriptor (%d)", rc);
                return rc;
            }
            if ((rc = read_mq_descriptor(in, &wakeLockDesc)) != STATUS_OK) {
                ALOGE("initialize: failed to read wakeLockDescriptor (%d)", rc);
                return rc;
            }
            if ((rc = AParcel_readStrongBinder(in, &cb)) != STATUS_OK) {
                ALOGE("initialize: failed to read sensorsCallback (%d)", rc);
                return rc;
            }

            ALOGI("initialize: eventQ(grantors=%zu fds=%zu quantum=%d) wakeLockQ(grantors=%zu fds=%zu quantum=%d)",
                  eventDesc.grantors.size(), eventDesc.fds.size(), eventDesc.quantum,
                  wakeLockDesc.grantors.size(), wakeLockDesc.fds.size(), wakeLockDesc.quantum);

            pthread_mutex_lock(&st->lock);
            /* Disable all sensors on re-initialize (per AIDL ISensors spec) */
            for (auto& s : st->sensors) {
                s.enabled = false;
                s.hasLastValue = false;
            }
            if (st->callback) {
                AIBinder_decStrong(st->callback);
            }
            st->callback = cb; /* takes ownership of ref from readStrongBinder */
            pthread_mutex_unlock(&st->lock);

            pthread_mutex_lock(&st->fmqLock);
            bool ok = st->eventQueue.init(eventDesc, true);
            if (ok && wakeLockDesc.grantors.size() >= 3) {
                st->wakeLockQueue.init(wakeLockDesc, false);
            } else {
                st->wakeLockQueue.reset();
            }
            pthread_mutex_unlock(&st->fmqLock);

            eventDesc.closeFds();
            wakeLockDesc.closeFds();

            if (!ok) {
                ALOGE("initialize: failed to map event FMQ");
                return write_service_specific(out, kErrBadValue);
            }

            pthread_mutex_lock(&st->lock);
            if (!st->pollStarted) {
                if (pthread_create(&st->pollThread, nullptr, poll_thread_fn, st) == 0) {
                    st->pollStarted = true;
                } else {
                    ALOGE("initialize: pthread_create failed: %s", strerror(errno));
                }
            }
            pthread_mutex_unlock(&st->lock);

            ALOGI("initialize: FMQ mapped (ringSize=%zu, quantum=%zu, evFlag=%p)",
                  st->eventQueue.ringSize, st->eventQueue.quantum, (void*)st->eventQueue.evFlagWord);
            return write_ok(out);
        }

        case TX_SET_OPERATION_MODE: {
            int32_t mode = 0;
            if ((rc = AParcel_readInt32(in, &mode)) != STATUS_OK) return rc;
            if (mode == 0) { /* NORMAL */
                return write_ok(out);
            }
            return write_exception(out, EX_UNSUPPORTED_OPERATION);
        }

        case TX_CONFIG_DIRECT_REPORT:
        case TX_INJECT_SENSOR_DATA:
        case TX_REGISTER_DIRECT_CHANNEL:
        case TX_UNREGISTER_DIRECT_CHANNEL:
            return write_exception(out, EX_UNSUPPORTED_OPERATION);

        case TX_GET_INTERFACE_VERSION: {
            if ((rc = write_ok(out)) != STATUS_OK) return rc;
            return AParcel_writeInt32(out, kSensorsVersion);
        }

        case TX_GET_INTERFACE_HASH: {
            if ((rc = write_ok(out)) != STATUS_OK) return rc;
            return AParcel_writeString(out, kSensorsHash, (int32_t)strlen(kSensorsHash));
        }

        default:
            return STATUS_UNKNOWN_TRANSACTION;
    }
}

/* =========================================================================
 * Main entry point
 * ========================================================================= */

static void init_sensors_table(SensorsState* st) {
    /* 0: Accelerometer (bcm4775-accel) */
    st->sensors[0] = {
        .info = {
            .sensorHandle           = HANDLE_ACCEL,
            .name                   = "BCM4775 Accelerometer",
            .vendor                 = "Broadcom/Mainline",
            .version                = 1,
            .type                   = SENSOR_TYPE_ACCELEROMETER,
            .typeAsString           = "android.sensor.accelerometer",
            .maxRange               = 78.4532f,      /* +-8g */
            .resolution             = 0.002394202f,  /* updated from in_accel_scale */
            .power                  = 0.25f,
            .minDelayUs             = 5000,          /* 200 Hz */
            .fifoReservedEventCount = 0,
            .fifoMaxEventCount      = 0,
            .requiredPermission     = "",
            .maxDelayUs             = 200000,        /* 5 Hz */
            .flags                  = kFlagContinuousMode,
        },
        .iioName      = "bcm4775-accel",
        .iioPath      = "",
        .present      = false,
        .enabled      = false,
        .onChange     = false,
        .wakeUp       = false,
        .hasLastValue = false,
        .lastValue    = 0.0f,
        .scale        = 0.002394202,
        .periodNs     = 20000000LL, /* 50 Hz default */
        .nextPollNs   = 0,
    };

    /* 1: Ambient Light (bcm4775-light) */
    st->sensors[1] = {
        .info = {
            .sensorHandle           = HANDLE_LIGHT,
            .name                   = "BCM4775 Ambient Light",
            .vendor                 = "Broadcom/Mainline",
            .version                = 1,
            .type                   = SENSOR_TYPE_LIGHT,
            .typeAsString           = "android.sensor.light",
            .maxRange               = 60000.0f,
            .resolution             = 1.0f,
            .power                  = 0.15f,
            .minDelayUs             = 0,
            .fifoReservedEventCount = 0,
            .fifoMaxEventCount      = 0,
            .requiredPermission     = "",
            .maxDelayUs             = 0,
            .flags                  = kFlagOnChangeMode,
        },
        .iioName      = "bcm4775-light",
        .iioPath      = "",
        .present      = false,
        .enabled      = false,
        .onChange     = true,
        .wakeUp       = false,
        .hasLastValue = false,
        .lastValue    = 0.0f,
        .scale        = 1.0,
        .periodNs     = 200000000LL, /* 200 ms poll */
        .nextPollNs   = 0,
    };

    /* 2: Proximity (bcm4775-prox) */
    st->sensors[2] = {
        .info = {
            .sensorHandle           = HANDLE_PROX,
            .name                   = "BCM4775 Proximity",
            .vendor                 = "Broadcom/Mainline",
            .version                = 1,
            .type                   = SENSOR_TYPE_PROXIMITY,
            .typeAsString           = "android.sensor.proximity",
            .maxRange               = 5.0f,
            .resolution             = 5.0f,
            .power                  = 0.5f,
            .minDelayUs             = 0,
            .fifoReservedEventCount = 0,
            .fifoMaxEventCount      = 0,
            .requiredPermission     = "",
            .maxDelayUs             = 0,
            .flags                  = kFlagOnChangeMode | kFlagWakeUp,
        },
        .iioName      = "bcm4775-prox",
        .iioPath      = "",
        .present      = false,
        .enabled      = false,
        .onChange     = true,
        .wakeUp       = true,
        .hasLastValue = false,
        .lastValue    = 0.0f,
        .scale        = 1.0,
        .periodNs     = 200000000LL, /* 200 ms poll */
        .nextPollNs   = 0,
    };

    /* 3: Compass Heading (bcm4775-compass) */
    st->sensors[3] = {
        .info = {
            .sensorHandle           = HANDLE_COMPASS,
            .name                   = "BCM4775 Compass Heading",
            .vendor                 = "Broadcom/Mainline",
            .version                = 1,
            .type                   = SENSOR_TYPE_HEADING,
            .typeAsString           = "android.sensor.heading",
            .maxRange               = 360.0f,
            .resolution             = 0.001f,        /* updated from in_rot_scale */
            .power                  = 0.5f,
            .minDelayUs             = 50000,         /* 20 Hz */
            .fifoReservedEventCount = 0,
            .fifoMaxEventCount      = 0,
            .requiredPermission     = "",
            .maxDelayUs             = 1000000,       /* 1 Hz */
            .flags                  = kFlagContinuousMode,
        },
        .iioName      = "bcm4775-compass",
        .iioPath      = "",
        .present      = false,
        .enabled      = false,
        .onChange     = false,
        .wakeUp       = false,
        .hasLastValue = false,
        .lastValue    = 0.0f,
        .scale        = 0.001,
        .periodNs     = 100000000LL, /* 10 Hz default */
        .nextPollNs   = 0,
    };
}

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(4);

    SensorsState state;
    pthread_mutex_init(&state.lock, nullptr);
    pthread_mutex_init(&state.fmqLock, nullptr);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&state.cond, &cattr);
    pthread_condattr_destroy(&cattr);

    state.pollStarted = false;
    state.stop        = false;
    state.callback    = nullptr;

    init_sensors_table(&state);
    discover_iio_devices_locked(&state);

    AIBinder_Class* clazz = AIBinder_Class_define(kSensorsDescriptor, on_create, on_destroy, on_transact);
    if (!clazz) {
        ALOGE("Failed to define AIBinder_Class for %s", kSensorsDescriptor);
        return 1;
    }

    AIBinder* binder = AIBinder_new(clazz, &state);
    if (!binder) {
        ALOGE("Failed to create AIBinder");
        return 1;
    }

    AIBinder_markVintfStability(binder);

    binder_status_t status = AServiceManager_addService(binder, kSensorsInstance);
    if (status != STATUS_OK) {
        ALOGE("AServiceManager_addService(%s) failed: %d", kSensorsInstance, status);
        AIBinder_decStrong(binder);
        return 1;
    }

    ALOGI("Registered %s (mainline BCM4775 IIO sensors HAL v%d)", kSensorsInstance, kSensorsVersion);
    ABinderProcess_joinThreadPool();

    pthread_mutex_lock(&state.lock);
    state.stop = true;
    pthread_cond_broadcast(&state.cond);
    bool joined = state.pollStarted;
    pthread_mutex_unlock(&state.lock);

    if (joined) {
        pthread_join(state.pollThread, nullptr);
    }
    if (state.callback) {
        AIBinder_decStrong(state.callback);
    }
    AIBinder_decStrong(binder);
    return 0;
}
