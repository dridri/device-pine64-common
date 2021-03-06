// The MIT License (MIT)
// Copyright (c) 2016 Kamil Trzciński <ayufan@ayufan.eu>

// Permission is hereby granted, free of charge,
// to any person obtaining a copy of this software
// and associated documentation files (the "Software"),
// to deal in the Software without restriction,
// including without limitation the rights to
// use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software,
// and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice
// shall be included in all copies or substantial portions
// of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#define LOG_TAG "sunxi-hdmi-cec"

#include <hardware/hdmi_cec.h>

#include <stdlib.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <memory.h>
#include <errno.h>
#include "log.h"

#define HDMICEC_IOC_MAGIC  'H'
#define HDMICEC_IOC_SETLOGICALADDRESS _IOW(HDMICEC_IOC_MAGIC,  1, unsigned char)
#define HDMICEC_IOC_STARTDEVICE _IO(HDMICEC_IOC_MAGIC,  2)
#define HDMICEC_IOC_STOPDEVICE  _IO(HDMICEC_IOC_MAGIC,  3)
#define HDMICEC_IOC_GETPHYADDRESS _IOR(HDMICEC_IOC_MAGIC,  4, unsigned char[4])
#define HDMICEC_IOC_SETWAKEUP   _IOR(HDMICEC_IOC_MAGIC,  10, unsigned char)

#define CEC_SUNXI_PATH "/dev/sunxi_hdmi_cec"
#define CEC_VENDOR_PULSE_EIGHT 0x001582
#define CEC_VERSION_1_4 0x05

#define MESSAGE_TYPE_RECEIVE_SUCCESS            1
#define MESSAGE_TYPE_NOACK              2
#define MESSAGE_TYPE_DISCONNECTED               3
#define MESSAGE_TYPE_CONNECTED          4
#define MESSAGE_TYPE_SEND_SUCCESS               5

typedef struct hdmi_cec_event {
    int event_type;
    int msg_len;
    unsigned char msg[17];
} hdmi_cec_event_t;

static int sunxi_hdmi_cec = -1;
static int enabled = 0;
static int powered = 0;
static pthread_t process_thread_handle = -1;
static event_callback_t callback_func;
static void *callback_arg;
static cec_logical_address_t logical_address = CEC_DEVICE_INACTIVE;

static void get_vendor_id(const struct hdmi_cec_device *dev, uint32_t *vendor_id) {
    *vendor_id = CEC_VENDOR_PULSE_EIGHT;
}

static int add_logical_address(const struct hdmi_cec_device *dev, cec_logical_address_t addr) {
    if (logical_address == addr) {
        return 0;
    }
    int ret = ioctl(sunxi_hdmi_cec, HDMICEC_IOC_SETLOGICALADDRESS, addr);
    if (ret == 0) {
        logical_address = addr;
        ALOGV("add_logical_address: %d", addr);
        return 0;
    } else {
        ALOGE("add_logical_address: %d failed: %d", addr, errno);
        return -errno;
    }
}

static void clear_logical_address(const struct hdmi_cec_device *dev) {
    add_logical_address(dev, 15);
}

static int get_physical_address(const struct hdmi_cec_device *dev, uint16_t *addr) {
    int ret = ioctl(sunxi_hdmi_cec, HDMICEC_IOC_GETPHYADDRESS, addr);
    if (ret == 0) {
        ALOGV("get_physical_address: %d", *addr);
        return 0;
    } else {
        ALOGE("get_physical_address: failed: %d", ret);
        return -errno;
    }
}

static int send_message(const struct hdmi_cec_device *dev, const cec_message_t *msg) {
    if (sunxi_hdmi_cec < 0) {
        ALOGE("send_message: not ready");
        return HDMI_RESULT_FAIL;
    }

    unsigned char message[CEC_MESSAGE_BODY_MAX_LENGTH + 1];
    message[0] = (msg->initiator << 4) | (msg->destination & 0x0f);
    memcpy(message + 1, msg->body, msg->length);

    int ret = write(sunxi_hdmi_cec, message, msg->length + 1);
    if (ret >= 0) {
        ALOGV("hdmi-cec sent initiator=%d destination=%d length=%zu msg=%02x %02x %02x",
              msg->initiator, msg->destination, msg->length,
              msg->body[0], msg->body[1], msg->body[2]);
        return HDMI_RESULT_SUCCESS;
    }

    ALOGW("hdmi-cec sent failed initiator=%d destination=%d length=%zu msg=%02x %02x %02x errno=%d",
          msg->initiator, msg->destination, msg->length,
          msg->body[0], msg->body[1], msg->body[2],
          errno);

    if (errno == EBUSY) {
        return HDMI_RESULT_BUSY;
    } else if (errno == EIO) {
        return HDMI_RESULT_NACK;
    } else {
        return HDMI_RESULT_FAIL;
    }
}

static void hotplug_event(struct hdmi_cec_device *dev, int port_id, int connected) {
    hdmi_event_t event;
    event.type = HDMI_EVENT_HOT_PLUG;
    event.dev = dev;
    event.hotplug.port_id = port_id;
    event.hotplug.connected = connected;
    powered = connected;

    ALOGI("hdmi-hotplug: port_id=%d connected=%d",
          port_id, connected);

    if (callback_func) {
        callback_func(&event, callback_arg);
    }
}

static int send_cec_message(struct hdmi_cec_device *dev, int initiator, int destination, const unsigned char *data,
                            size_t length) {
    cec_message_t msg;
    msg.initiator = initiator;
    msg.destination = destination;
    msg.length = length;
    memcpy(msg.body, data, length);
    return send_message(dev, &msg);
}

static int
handle_cec_opcode(struct hdmi_cec_device *dev, int initiator, int destination,
                  int opcode, const unsigned char *data, size_t length) {
    switch (opcode) {
        case CEC_MESSAGE_GIVE_DECK_STATUS: {
            unsigned char data[] = {CEC_MESSAGE_DECK_STATUS, 0x20};
            send_cec_message(dev, destination, initiator, data, 2);
            return 1;
        }

        case CEC_MESSAGE_DEVICE_VENDOR_ID: {
            if (initiator != CEC_DEVICE_TV) {
                break;
            }
            if (!powered) {
                hotplug_event(dev, 0, 1);
            }

            // We broadcast our vendor ID
            uint32_t vendor_id = 0;
            get_vendor_id(dev, &vendor_id);

            unsigned char data[] = {
                    CEC_MESSAGE_DEVICE_VENDOR_ID,
                    vendor_id >> 16,
                    vendor_id >> 8,
                    vendor_id
            };
            send_cec_message(dev, logical_address, 15, data, 4);
            return 0;
        }
    }
    return 0;
}

static void
cec_event(struct hdmi_cec_device *dev, int initiator, int destination, const unsigned char *data, size_t length) {
    if (length <= 0) {
        return;
    }

    hdmi_event_t event;
    event.type = HDMI_EVENT_CEC_MESSAGE;
    event.dev = dev;
    event.cec.initiator = initiator;
    event.cec.destination = destination;
    event.cec.length = length;
    memcpy(&event.cec.body, data, length);

    ALOGV("hdmi-cec received initiator=%d destination=%d length=%zu msg=%02x %02x %02x",
          event.cec.initiator, event.cec.destination, event.cec.length,
          event.cec.body[0], event.cec.body[1], event.cec.body[2]);

    if (length >= 1 && handle_cec_opcode(dev, initiator, destination, data[0], data + 1, length - 1)) {
        return;
    }

    if (callback_func) {
        callback_func(&event, callback_arg);
    }
}

static void register_event_callback(const struct hdmi_cec_device *dev,
                                    event_callback_t callback, void *arg) {
    callback_func = callback;
    callback_arg = arg;
    ALOGV("register_event_callback: %p", callback);
}

static void get_version(const struct hdmi_cec_device *dev, int *version) {
    *version = CEC_VERSION_1_4;
}

static void get_port_info(const struct hdmi_cec_device *dev,
                          struct hdmi_port_info *list[], int *total) {
    static hdmi_port_info_t info = {
            .type = HDMI_OUTPUT,
            .port_id = 0,
            .cec_supported = 1,
            .arc_supported = 0,
            .physical_address = 0,
    };

    *total = 1;
    list[0] = &info;
}

static void set_audio_return_channel(const struct hdmi_cec_device *dev, int port_id, int flag) {
    ALOGV("set_audio_return_channel: port_id=%d flag=%d", port_id, flag);
}

static int is_connected(const struct hdmi_cec_device *dev, int port_id) {
    return HDMI_CONNECTED; // or HDMI_NOT_CONNECTED
}

static int poll_data(int fd, int timeout_usec) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);

    struct timeval timeout = {0, timeout_usec};

    return select(fd + 1, &set, NULL, NULL, &timeout);
}

static void handle_cec_event(struct hdmi_cec_device *dev, const hdmi_cec_event_t *event) {
    switch (event->event_type) {
        case MESSAGE_TYPE_RECEIVE_SUCCESS:
            if (event->msg_len >= 1) {
                cec_event(dev, event->msg[0] >> 4,
                          event->msg[0] & 0x0f,
                          event->msg + 1,
                          event->msg_len - 1);
            }
            break;

        case MESSAGE_TYPE_CONNECTED:
            hotplug_event(dev, 0, 1);
            break;

        case MESSAGE_TYPE_DISCONNECTED:
            hotplug_event(dev, 0, 0);
            break;

        default:
            ALOGW("handle_cec_event: unsupported event: type=%d, msg_len=%d", event->event_type, event->msg_len);
            break;
    }
}

static void *process_thread(void *dev) {
    while (sunxi_hdmi_cec >= 0) {
        int ret = poll_data(sunxi_hdmi_cec, 100000);
        if (ret == -1) {
            usleep(500 * 1000);
            ALOGW("failed to receive data");
            continue;
        } else if (ret == 0) {
            continue;
        }

        hdmi_cec_event_t event;
        ret = read(sunxi_hdmi_cec, &event, sizeof(event));
        if (ret <= 0) {
            ALOGW("invalid data receeived: ret=%d msg_len=%d", ret, event.msg_len);
            continue;
        }

        handle_cec_event(dev, &event);
    }
    return NULL;
}

static int enable_hdmi_cec(const struct hdmi_cec_device *dev) {
    if (enabled) {
        ALOGV("enable_hdmi_cec: is already enabled");
        return 0;
    }
    int ret = ioctl(sunxi_hdmi_cec, HDMICEC_IOC_STARTDEVICE, NULL);
    if (ret < 0) {
        ALOGW("enable_hdmi_cec: failed=%d", ret);
    } else {
        ALOGV("enable_hdmi_cec: enabled");
        enabled = 1;
    }
    return ret;
}

static int disable_hdmi_cec(const struct hdmi_cec_device *dev) {
    if (!enabled) {
        ALOGV("disable_hdmi_cec: is already disabled");
        return 0;
    }
    int ret = ioctl(sunxi_hdmi_cec, HDMICEC_IOC_STOPDEVICE, NULL);
    if (ret < 0) {
        ALOGW("disable_hdmi_cec: failed=%d", ret);
    } else {
        ALOGV("disable_hdmi_cec: disabled");
        enabled = 0;
    }
    return ret;
}

static int open_hdmi_cec(struct hdmi_cec_device *dev) {
    if (sunxi_hdmi_cec >= 0) {
        ALOGV("open_hdmi_cec: is opened fd=%d", sunxi_hdmi_cec);
        return 0;
    }

    sunxi_hdmi_cec = open(CEC_SUNXI_PATH, O_RDWR);
    if (sunxi_hdmi_cec < 0) {
        ALOGE("open_hdmi_cec: unable to open device=%d", errno);
        return -1;
    }

    int ret = pthread_create(&process_thread_handle, NULL, process_thread, dev);
    if (ret < 0) {
        ALOGE("open_hdmi_cec: unable to start thread=%d", ret);
        close(sunxi_hdmi_cec);
        sunxi_hdmi_cec = -1;
        return -1;
    }

    ALOGV("open_hdmi_cec: opened fd=%d", sunxi_hdmi_cec);
    return 0;
}

static int close_hdmi_cec(struct hdmi_cec_device *dev) {
    if (sunxi_hdmi_cec < 0) {
        ALOGV("close_hdmi_cec: is closed");
        return 0;
    }

    ALOGV("close_hdmi_cec: fd=%d", sunxi_hdmi_cec);

    disable_hdmi_cec(dev);
    close(sunxi_hdmi_cec);
    sunxi_hdmi_cec = -1;

    pthread_join(process_thread_handle, NULL);
    process_thread_handle = 0;
    return 0;
}

static int set_hdmi_cec_wake_up(const struct hdmi_cec_device *dev, unsigned char enabled) {
    int ret = ioctl(sunxi_hdmi_cec, HDMICEC_IOC_SETWAKEUP, enabled);
    if (ret < 0) {
        ALOGW("set_hdmi_cec_wake_up: enabled=%d failed=%d", enabled, ret);
    } else {
        ALOGV("set_hdmi_cec_wake_up: changed enabled=%d", enabled);
    }
    return ret;
}

static void set_option(struct hdmi_cec_device *dev, int flag, int value) {
    switch (flag) {
        case HDMI_OPTION_WAKEUP:
            set_hdmi_cec_wake_up(dev, value);
            break;

        case HDMI_OPTION_ENABLE_CEC:
            if (value) {
                open_hdmi_cec(dev);
            } else {
                close_hdmi_cec(dev);
            }
            break;

        case HDMI_OPTION_SYSTEM_CEC_CONTROL:
            if (value) {
                enable_hdmi_cec(dev);
            } else {
                disable_hdmi_cec(dev);
            }
            break;

        default:
            ALOGV("set_option: flag=%d value=%d unsupported", flag, value);
            break;
    }
}

static int open_hw_module(const struct hw_module_t *module, char const *name,
                         struct hw_device_t **device) {
    ALOGV("open_hw_module");

    hdmi_cec_device_t *dev = (hdmi_cec_device_t *) malloc(sizeof(hdmi_cec_device_t));
    if (dev == NULL) {
        ALOGE("failed to allocate");
        return -1;
    }

    memset(dev, 0, sizeof(*dev));
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t *) module;
    dev->common.close = close_hdmi_cec;
    dev->add_logical_address = add_logical_address;
    dev->clear_logical_address = clear_logical_address;
    dev->get_physical_address = get_physical_address;
    dev->send_message = send_message;
    dev->register_event_callback = register_event_callback;
    dev->get_version = get_version;
    dev->get_vendor_id = get_vendor_id;
    dev->get_port_info = get_port_info;
    dev->set_option = set_option;
    dev->set_audio_return_channel = set_audio_return_channel;
    dev->is_connected = is_connected;

    int ret = open_hdmi_cec(dev);
    if (ret < 0) {
        ALOGE("open_hw_module: failed: %d", ret);
        free(dev);
        return ret;
    }

    enable_hdmi_cec(dev);

    set_hdmi_cec_wake_up(dev, 1);

    *device = (struct hw_device_t *) dev;

    ALOGV("open_hw_module: success");
    return 0;
}

static struct hw_module_methods_t hdmi_cec_module_methods = {
    .open = open_hw_module
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = 1,
    .hal_api_version = 0,
    .id = HDMI_CEC_HARDWARE_MODULE_ID,
    .name = "sunxi hdmi cec module",
    .author = "Kamil Trzcinski <ayufan@ayufan.eu>",
    .methods = &hdmi_cec_module_methods,
};
