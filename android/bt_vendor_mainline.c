/*
 * Mainline Linux HCI_CHANNEL_USER bt-vendor bridge for Android HIDL bluetooth@1.0.
 *
 * Stock /vendor/lib64/hw/android.hardware.bluetooth@1.0-impl.so dlopens
 * /vendor/lib64/libbt-vendor.so and expects a byte-stream H4 file descriptor
 * from BT_VND_OP_USERIAL_OPEN. Linux's mainline hci_bcm driver already owns
 * /dev/ttySAC1, loads BCM4362A2.hcd, and exposes hci0 via AF_BLUETOOTH
 * HCI_CHANNEL_USER (packet-framed H4).
 *
 * This library binds hci0 in HCI_CHANNEL_USER mode and bridges packet-framed
 * H4 <-> stream H4 over an AF_UNIX SOCK_STREAM socketpair so stock Android 16
 * Bluetooth works unmodified on Mainline Linux 7.2.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <android/log.h>

#define LOG_TAG "bt_vendor_mainline"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Linux Bluetooth HCI constants */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_HCI       1
#define HCI_CHANNEL_USER  1
#define HCIDEVDOWN        _IOW('H', 202, int)

struct sockaddr_hci {
    sa_family_t    hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

/* AOSP hardware/libhardware/include/hardware/bt_vendor_lib.h ABI */
typedef enum {
    BT_VND_OP_POWER_CTRL,
    BT_VND_OP_FW_CFG,
    BT_VND_OP_SCO_CFG,
    BT_VND_OP_USERIAL_OPEN,
    BT_VND_OP_USERIAL_CLOSE,
    BT_VND_OP_GET_LPM_IDLE_TIMEOUT,
    BT_VND_OP_LPM_SET_MODE,
    BT_VND_OP_LPM_WAKE_SET_STATE,
    BT_VND_OP_SET_AUDIO_STATE,
    BT_VND_OP_EPILOG,
    BT_VND_OP_A2DP_OFFLOAD_START,
    BT_VND_OP_A2DP_OFFLOAD_STOP,
} bt_vendor_opcode_t;

typedef enum {
    BT_VND_OP_RESULT_SUCCESS,
    BT_VND_OP_RESULT_FAIL,
} bt_vendor_op_result_t;

typedef void (*cfg_result_cb)(bt_vendor_op_result_t result);
typedef void *(*malloc_cb)(int size);
typedef void (*mdealloc_cb)(void *p_buf);
typedef uint8_t (*cmd_xmit_cb)(uint16_t opcode, void *p_buf, void *p_cback);
typedef void (*tUSERIAL_CALLBACK)(bt_vendor_op_result_t result);
typedef void (*cfg_a2dp_cb)(bt_vendor_op_result_t result, bt_vendor_opcode_t op, uint8_t bta_av_handle);

typedef struct {
    size_t size;
    cfg_result_cb fwcfg_cb;
    cfg_result_cb scocfg_cb;
    cfg_result_cb lpm_cb;
    cfg_result_cb audio_state_cb;
    malloc_cb alloc;
    mdealloc_cb dealloc;
    cmd_xmit_cb xmit_cb;
    tUSERIAL_CALLBACK epilog_cb;
    cfg_a2dp_cb a2dp_offload_cb;
} bt_vendor_callbacks_t;

typedef struct {
    size_t size;
    int (*init)(const bt_vendor_callbacks_t *p_cb, unsigned char *local_bdaddr);
    int (*op)(bt_vendor_opcode_t opcode, void *param);
    void (*cleanup)(void);
} bt_vendor_interface_t;

static const bt_vendor_callbacks_t *g_cb = NULL;
static int g_hci_fd = -1;
static int g_unix_fd = -1;
static volatile int g_running = 0;
static pthread_t g_tx_thread;
static pthread_t g_rx_thread;
static int g_threads_started = 0;

static int read_exact(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

static int write_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Kernel hci0 (packet-framed H4) -> Android bluetooth@1.0-impl (stream H4) */
static void *rx_relay_fn(void *arg)
{
    (void)arg;
    uint8_t buf[4096];

    while (g_running) {
        ssize_t n = read(g_hci_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (g_running)
                ALOGE("hci read failed: %s", strerror(errno));
            break;
        }
        if (n == 0)
            break;
        if (write_all(g_unix_fd, buf, (size_t)n) < 0) {
            if (g_running)
                ALOGE("unix write failed: %s", strerror(errno));
            break;
        }
    }
    return NULL;
}

/* Android bluetooth@1.0-impl (stream H4) -> Kernel hci0 (packet-framed H4) */
static void *tx_relay_fn(void *arg)
{
    (void)arg;
    uint8_t buf[4096];

    while (g_running) {
        if (read_exact(g_unix_fd, buf, 1) < 0)
            break;

        uint8_t type = buf[0];
        size_t hdr_len = 0;
        size_t pay_len = 0;

        switch (type) {
        case 0x01: /* HCI Command: 2B opcode + 1B param len */
            hdr_len = 3;
            if (read_exact(g_unix_fd, buf + 1, hdr_len) < 0)
                goto done;
            pay_len = buf[3];
            break;
        case 0x02: /* HCI ACL Data: 2B handle + 2B len */
            hdr_len = 4;
            if (read_exact(g_unix_fd, buf + 1, hdr_len) < 0)
                goto done;
            pay_len = (size_t)buf[3] | ((size_t)buf[4] << 8);
            break;
        case 0x03: /* HCI SCO Data: 2B handle + 1B len */
            hdr_len = 3;
            if (read_exact(g_unix_fd, buf + 1, hdr_len) < 0)
                goto done;
            pay_len = buf[3];
            break;
        case 0x05: /* HCI ISO Data: 2B handle + 2B len (14-bit) */
            hdr_len = 4;
            if (read_exact(g_unix_fd, buf + 1, hdr_len) < 0)
                goto done;
            pay_len = ((size_t)buf[3] | ((size_t)buf[4] << 8)) & 0x3fff;
            break;
        default:
            ALOGW("unexpected H4 TX packet type 0x%02x", type);
            continue;
        }

        if (1 + hdr_len + pay_len > sizeof(buf)) {
            ALOGE("H4 TX packet too large: %zu", 1 + hdr_len + pay_len);
            break;
        }
        if (pay_len > 0 && read_exact(g_unix_fd, buf + 1 + hdr_len, pay_len) < 0)
            break;

        size_t total = 1 + hdr_len + pay_len;
        ssize_t n;
        do {
            n = write(g_hci_fd, buf, total);
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            if (g_running)
                ALOGE("hci write (%zu bytes) failed: %s", total, strerror(errno));
            break;
        }
    }
done:
    return NULL;
}

static void stop_relay(void)
{
    g_running = 0;
    if (g_unix_fd >= 0)
        shutdown(g_unix_fd, SHUT_RDWR);
    if (g_hci_fd >= 0)
        shutdown(g_hci_fd, SHUT_RDWR);
    if (g_threads_started) {
        pthread_join(g_tx_thread, NULL);
        pthread_join(g_rx_thread, NULL);
        g_threads_started = 0;
    }
    if (g_unix_fd >= 0) {
        close(g_unix_fd);
        g_unix_fd = -1;
    }
    if (g_hci_fd >= 0) {
        close(g_hci_fd);
        g_hci_fd = -1;
    }
}

static int open_hci_user_channel(void)
{
    int ctl = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (ctl < 0) {
        ALOGE("socket(AF_BLUETOOTH) failed: %s", strerror(errno));
        return -1;
    }
    if (ioctl(ctl, HCIDEVDOWN, 0) < 0 && errno != EALREADY) {
        ALOGW("ioctl(HCIDEVDOWN, 0): %s", strerror(errno));
    }
    close(ctl);

    int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    if (fd < 0) {
        ALOGE("socket(HCI_CHANNEL_USER) failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_hci addr;
    memset(&addr, 0, sizeof(addr));
    addr.hci_family = AF_BLUETOOTH;
    addr.hci_dev = 0;
    addr.hci_channel = HCI_CHANNEL_USER;

    for (int tries = 0; tries < 20; tries++) {
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            ALOGI("bound hci0 in HCI_CHANNEL_USER mode");
            return fd;
        }
        if (errno != EBUSY && errno != EAGAIN)
            break;
        usleep(100000);
    }

    ALOGE("bind(hci0, HCI_CHANNEL_USER) failed: %s", strerror(errno));
    close(fd);
    return -1;
}

static int bt_vnd_init(const bt_vendor_callbacks_t *p_cb, unsigned char *local_bdaddr)
{
    (void)local_bdaddr;
    g_cb = p_cb;
    ALOGI("init");
    return 0;
}

static int bt_vnd_op(bt_vendor_opcode_t opcode, void *param)
{
    switch (opcode) {
    case BT_VND_OP_POWER_CTRL:
        return 0;

    case BT_VND_OP_USERIAL_OPEN: {
        int *fd_array = (int *)param;
        stop_relay();

        g_hci_fd = open_hci_user_channel();
        if (g_hci_fd < 0)
            return -1;

        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
            ALOGE("socketpair failed: %s", strerror(errno));
            close(g_hci_fd);
            g_hci_fd = -1;
            return -1;
        }

        g_unix_fd = sv[1];
        g_running = 1;
        if (pthread_create(&g_rx_thread, NULL, rx_relay_fn, NULL) != 0 ||
            pthread_create(&g_tx_thread, NULL, tx_relay_fn, NULL) != 0) {
            ALOGE("pthread_create failed");
            close(sv[0]);
            stop_relay();
            return -1;
        }
        g_threads_started = 1;
        fd_array[0] = sv[0];
        ALOGI("USERIAL_OPEN returning bridged H4 fd=%d", sv[0]);
        return 1;
    }

    case BT_VND_OP_USERIAL_CLOSE:
        ALOGI("USERIAL_CLOSE");
        stop_relay();
        return 0;

    case BT_VND_OP_FW_CFG:
        ALOGI("FW_CFG -> immediate SUCCESS (kernel hci_bcm loaded firmware)");
        if (g_cb && g_cb->fwcfg_cb)
            g_cb->fwcfg_cb(BT_VND_OP_RESULT_SUCCESS);
        return 0;

    case BT_VND_OP_SCO_CFG:
        if (g_cb && g_cb->scocfg_cb)
            g_cb->scocfg_cb(BT_VND_OP_RESULT_SUCCESS);
        return 0;

    case BT_VND_OP_GET_LPM_IDLE_TIMEOUT:
        if (param)
            *(uint32_t *)param = 3000;
        return 0;

    case BT_VND_OP_LPM_SET_MODE:
        if (g_cb && g_cb->lpm_cb)
            g_cb->lpm_cb(BT_VND_OP_RESULT_SUCCESS);
        return 0;

    case BT_VND_OP_LPM_WAKE_SET_STATE:
    case BT_VND_OP_SET_AUDIO_STATE:
        return 0;

    case BT_VND_OP_EPILOG:
        if (g_cb && g_cb->epilog_cb)
            g_cb->epilog_cb(BT_VND_OP_RESULT_SUCCESS);
        return 0;

    default:
        return 0;
    }
}

static void bt_vnd_cleanup(void)
{
    ALOGI("cleanup");
    stop_relay();
    g_cb = NULL;
}

__attribute__((visibility("default")))
const bt_vendor_interface_t BLUETOOTH_VENDOR_LIB_INTERFACE = {
    .size    = sizeof(bt_vendor_interface_t),
    .init    = bt_vnd_init,
    .op      = bt_vnd_op,
    .cleanup = bt_vnd_cleanup,
};
