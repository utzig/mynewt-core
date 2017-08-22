/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <syscfg/syscfg.h>
#include "sysinit/sysinit.h"
#include "sysflash/sysflash.h"
#include <os/os.h>
#include <bsp/bsp.h>
#include <hal/hal_gpio.h>
#include <hal/hal_flash.h>
#include <console/console.h>
#include <log/log.h>
#include <stats/stats.h>
#include "flash_map/flash_map.h"
#include <hal/hal_system.h>
#if MYNEWT_VAL(SPLIT_LOADER)
#include "split/split.h"
#endif
#include <assert.h>
#include <string.h>
#include <os/os_time.h>

#include <stdio.h>

#include <usb/usb.h>
#include <dev/class.h>
#include <dev/dev.h>
#include <hid/hid.h>

#include <hal_usb/hal_usb.h>

#ifdef ARCH_sim
#include <mcu/mcu_sim.h>
#endif

#define BLINKER_PRIO                       8
#define BLINKER_STACK_SIZE                 OS_STACK_ALIGN(192)
static struct os_task blinker_task;

#define USB_DEV_TASK_PRIO                  7
#define USB_DEV_STACK_SIZE                 OS_STACK_ALIGN(2048)
static struct os_task usb_dev_task;

#define CONTROLLER_ID kUSB_ControllerKhci0

#define USB_DEVICE_SPECIFIC_BCD_VERSION            0x0200
#define USB_DEVICE_DEMO_BCD_VERSION                0x0101

#define USB_DEVICE_CLASS                           0x00
#define USB_DEVICE_SUBCLASS                        0x00
#define USB_DEVICE_PROTOCOL                        0x00

#define USB_DEVICE_MAX_POWER                       0x32

#define USB_DESCRIPTOR_LENGTH_CONFIGURATION_ALL    41
#define USB_DESCRIPTOR_LENGTH_HID_GENERIC_REPORT   33
#define USB_DESCRIPTOR_LENGTH_HID                  9
#define USB_DESCRIPTOR_LENGTH_STRING0              4
#define USB_DESCRIPTOR_LENGTH_STRING1              38
#define USB_DESCRIPTOR_LENGTH_STRING2              38

#define USB_DEVICE_CONFIGURATION_COUNT             1
#define USB_DEVICE_STRING_COUNT                    3
#define USB_DEVICE_LANGUAGE_COUNT                  1

#define USB_HID_GENERIC_CONFIGURE_INDEX            1
#define USB_HID_GENERIC_INTERFACE_COUNT            1

#define USB_HID_GENERIC_IN_BUFFER_LENGTH           8
#define USB_HID_GENERIC_OUT_BUFFER_LENGTH          8
#define USB_HID_GENERIC_ENDPOINT_COUNT             2
#define USB_HID_GENERIC_INTERFACE_INDEX            0
#define USB_HID_GENERIC_ENDPOINT_IN                1
#define USB_HID_GENERIC_ENDPOINT_OUT               2

#define USB_HID_GENERIC_CLASS                      0x03
#define USB_HID_GENERIC_SUBCLASS                   0x00
#define USB_HID_GENERIC_PROTOCOL                   0x00

#define HS_HID_GENERIC_INTERRUPT_OUT_PACKET_SIZE   8
#define FS_HID_GENERIC_INTERRUPT_OUT_PACKET_SIZE   8
#define HS_HID_GENERIC_INTERRUPT_OUT_INTERVAL      0x04  /* 2^(4-1) = 1ms */
#define FS_HID_GENERIC_INTERRUPT_OUT_INTERVAL      0x01

#define HS_HID_GENERIC_INTERRUPT_IN_PACKET_SIZE    8
#define FS_HID_GENERIC_INTERRUPT_IN_PACKET_SIZE    8
#define HS_HID_GENERIC_INTERRUPT_IN_INTERVAL       0x04  /* 2^(4-1) = 1ms */
#define FS_HID_GENERIC_INTERRUPT_IN_INTERVAL       0x01

typedef struct
{
    usb_device_handle deviceHandle;
    class_handle_t hidHandle;
    //xTaskHandle applicationTaskHandle;
    //xTaskHandle deviceTaskHandle;
    uint8_t *buffer[2];
    uint8_t bufferIndex;
    uint8_t idleRate;
    uint8_t speed;
    uint8_t attach;
    uint8_t currentConfiguration;
    uint8_t currentInterfaceAlternateSetting[USB_HID_GENERIC_INTERFACE_COUNT];
} usb_hid_generic_struct_t;

static usb_status_t usb_dev_cb(usb_device_handle handle, uint32_t event, void *param);
static usb_status_t usb_dev_hid_cb(class_handle_t handle, uint32_t event, void *param);


static uint32_t s_GenericBuffer0[USB_HID_GENERIC_IN_BUFFER_LENGTH >> 2];
static uint32_t s_GenericBuffer1[USB_HID_GENERIC_IN_BUFFER_LENGTH >> 2];
usb_hid_generic_struct_t g_UsbDeviceHidGeneric;
extern usb_device_class_struct_t g_UsbDeviceHidGenericConfig;

usb_device_endpoint_struct_t g_UsbDeviceHidGenericEndpoints[USB_HID_GENERIC_ENDPOINT_COUNT] = {
    {
        USB_HID_GENERIC_ENDPOINT_IN | (USB_IN << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
        USB_ENDPOINT_INTERRUPT,
        FS_HID_GENERIC_INTERRUPT_IN_PACKET_SIZE,
    },
    {
        USB_HID_GENERIC_ENDPOINT_OUT | (USB_OUT << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
        USB_ENDPOINT_INTERRUPT,
        FS_HID_GENERIC_INTERRUPT_OUT_PACKET_SIZE,
    }
};

usb_device_interface_struct_t g_UsbDeviceHidGenericInterface[] = {
    {
        0,
        {
            USB_HID_GENERIC_ENDPOINT_COUNT,
            g_UsbDeviceHidGenericEndpoints,
        },
        NULL,
}};

usb_device_interfaces_struct_t g_UsbDeviceHidGenericInterfaces[USB_HID_GENERIC_INTERFACE_COUNT] = {
    {
        USB_HID_GENERIC_CLASS,
        USB_HID_GENERIC_SUBCLASS,
        USB_HID_GENERIC_PROTOCOL,
        USB_HID_GENERIC_INTERFACE_INDEX,
        g_UsbDeviceHidGenericInterface,
        sizeof(g_UsbDeviceHidGenericInterface) / sizeof(usb_device_interfaces_struct_t),
    },
};

usb_device_interface_list_t g_UsbDeviceHidGenericInterfaceList[USB_DEVICE_CONFIGURATION_COUNT] = {
    {
        USB_HID_GENERIC_INTERFACE_COUNT,
        g_UsbDeviceHidGenericInterfaces,
    },
};

usb_device_class_struct_t g_UsbDeviceHidGenericConfig = {
    g_UsbDeviceHidGenericInterfaceList,
    kUSB_DeviceClassTypeHid,
    USB_DEVICE_CONFIGURATION_COUNT,
};

uint8_t g_UsbDeviceDescriptor[USB_DESCRIPTOR_LENGTH_DEVICE] = {
    USB_DESCRIPTOR_LENGTH_DEVICE,
    USB_DESCRIPTOR_TYPE_DEVICE,
    USB_SHORT_GET_LOW(USB_DEVICE_SPECIFIC_BCD_VERSION),
    USB_SHORT_GET_HIGH(USB_DEVICE_SPECIFIC_BCD_VERSION),
    USB_DEVICE_CLASS,
    USB_DEVICE_SUBCLASS,
    USB_DEVICE_PROTOCOL,
    USB_CONTROL_MAX_PACKET_SIZE,
    0xC9, 0x1F,
    0x90, 0x00,
    USB_SHORT_GET_LOW(USB_DEVICE_DEMO_BCD_VERSION),
    USB_SHORT_GET_HIGH(USB_DEVICE_DEMO_BCD_VERSION),
    0x01,
    0x02,
    0x00,
    USB_DEVICE_CONFIGURATION_COUNT,
};

uint8_t g_UsbDeviceConfigurationDescriptor[USB_DESCRIPTOR_LENGTH_CONFIGURATION_ALL] = {
    USB_DESCRIPTOR_LENGTH_CONFIGURE,
    USB_DESCRIPTOR_TYPE_CONFIGURE,
    USB_SHORT_GET_LOW(USB_DESCRIPTOR_LENGTH_CONFIGURATION_ALL),
    USB_SHORT_GET_HIGH(USB_DESCRIPTOR_LENGTH_CONFIGURATION_ALL),
    USB_HID_GENERIC_INTERFACE_COUNT,
    USB_HID_GENERIC_CONFIGURE_INDEX,
    0x00,
    (USB_DESCRIPTOR_CONFIGURE_ATTRIBUTE_D7_MASK) |
        (MYNEWT_VAL(USB_DEVICE_CONFIG_SELF_POWER) << USB_DESCRIPTOR_CONFIGURE_ATTRIBUTE_SELF_POWERED_SHIFT) |
        (MYNEWT_VAL(USB_DEVICE_CONFIG_REMOTE_WAKEUP) << USB_DESCRIPTOR_CONFIGURE_ATTRIBUTE_REMOTE_WAKEUP_SHIFT),
    USB_DEVICE_MAX_POWER,
    USB_DESCRIPTOR_LENGTH_INTERFACE,
    USB_DESCRIPTOR_TYPE_INTERFACE,
    USB_HID_GENERIC_INTERFACE_INDEX,
    0x00,
    USB_HID_GENERIC_ENDPOINT_COUNT,
    USB_HID_GENERIC_CLASS,
    USB_HID_GENERIC_SUBCLASS,
    USB_HID_GENERIC_PROTOCOL,
    0x00,
    USB_DESCRIPTOR_LENGTH_HID,
    USB_DESCRIPTOR_TYPE_HID,
    0x00, 0x01,
    0x00,
    0x01,
    USB_DESCRIPTOR_TYPE_HID_REPORT,
    USB_SHORT_GET_LOW(USB_DESCRIPTOR_LENGTH_HID_GENERIC_REPORT),
    USB_SHORT_GET_HIGH(USB_DESCRIPTOR_LENGTH_HID_GENERIC_REPORT),
    USB_DESCRIPTOR_LENGTH_ENDPOINT,
    USB_DESCRIPTOR_TYPE_ENDPOINT,
    USB_HID_GENERIC_ENDPOINT_IN | (USB_IN << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
    USB_ENDPOINT_INTERRUPT,
    USB_SHORT_GET_LOW(FS_HID_GENERIC_INTERRUPT_IN_PACKET_SIZE),
    USB_SHORT_GET_HIGH(FS_HID_GENERIC_INTERRUPT_IN_PACKET_SIZE),
    FS_HID_GENERIC_INTERRUPT_IN_INTERVAL,
    USB_DESCRIPTOR_LENGTH_ENDPOINT,
    USB_DESCRIPTOR_TYPE_ENDPOINT,
    USB_HID_GENERIC_ENDPOINT_OUT | (USB_OUT << USB_DESCRIPTOR_ENDPOINT_ADDRESS_DIRECTION_SHIFT),
    USB_ENDPOINT_INTERRUPT,
    USB_SHORT_GET_LOW(FS_HID_GENERIC_INTERRUPT_OUT_PACKET_SIZE),
    USB_SHORT_GET_HIGH(FS_HID_GENERIC_INTERRUPT_OUT_PACKET_SIZE),
    FS_HID_GENERIC_INTERRUPT_OUT_INTERVAL,
};

uint8_t g_UsbDeviceHidGenericReportDescriptor[USB_DESCRIPTOR_LENGTH_HID_GENERIC_REPORT] = {
    0x05, 0x81, /* Usage Page (Vendor defined)*/
    0x09, 0x82, /* Usage (Vendor defined) */
    0xA1, 0x01, /* Collection (Application) */
    0x09, 0x83, /* Usage (Vendor defined) */

    0x09, 0x84, /* Usage (Vendor defined) */
    0x15, 0x80, /* logical Minimum (-128) */
    0x25, 0x7F, /* logical Maximum (127) */
    0x75, 0x08, /* Report Size (8U) */
    0x95, 0x08, /* Report Count (8U) */
    0x81, 0x02, /* Input(Data, Variable, Absolute) */

    0x09, 0x84, /* Usage (Vendor defined) */
    0x15, 0x80, /* logical Minimum (-128) */
    0x25, 0x7F, /* logical Maximum (127) */
    0x75, 0x08, /* Report Size (8U) */
    0x95, 0x08, /* Report Count (8U) */
    0x91, 0x02, /* Input(Data, Variable, Absolute) */
    0xC0,       /* end collection */
};

uint8_t g_UsbDeviceString0[USB_DESCRIPTOR_LENGTH_STRING0] = {
    sizeof(g_UsbDeviceString0),
    USB_DESCRIPTOR_TYPE_STRING,
    0x09,
    0x04,
};

uint8_t g_UsbDeviceString1[USB_DESCRIPTOR_LENGTH_STRING1] = {
    sizeof(g_UsbDeviceString1),
    USB_DESCRIPTOR_TYPE_STRING,
    'N', 0, 'X', 0, 'P', 0, ' ', 0, 'S', 0, 'E', 0, 'M', 0, 'I', 0,
    'C', 0, 'O', 0, 'N', 0, 'D', 0, 'U', 0, 'C', 0, 'T', 0, 'O', 0,
    'R', 0, 'S', 0,
};

uint8_t g_UsbDeviceString2[USB_DESCRIPTOR_LENGTH_STRING2] = {
    sizeof(g_UsbDeviceString2),
    USB_DESCRIPTOR_TYPE_STRING,
    'H', 0, 'I', 0, 'D', 0, ' ', 0, 'G', 0, 'E', 0, 'N', 0, 'E', 0,
    'R', 0, 'I', 0, 'C', 0, ' ', 0, 'D', 0, 'E', 0, 'V', 0, 'I', 0,
    'C', 0, 'E', 0,
};

uint32_t g_UsbDeviceStringDescriptorLength[USB_DEVICE_STRING_COUNT] = {
    sizeof(g_UsbDeviceString0),
    sizeof(g_UsbDeviceString1),
    sizeof(g_UsbDeviceString2),
};

uint8_t *g_UsbDeviceStringDescriptorArray[USB_DEVICE_STRING_COUNT] = {
    g_UsbDeviceString0,
    g_UsbDeviceString1,
    g_UsbDeviceString2,
};

usb_language_t g_UsbDeviceLanguage[USB_DEVICE_LANGUAGE_COUNT] = {
    {
        g_UsbDeviceStringDescriptorArray,
        g_UsbDeviceStringDescriptorLength,
        (uint16_t)0x0409,
    },
};

usb_language_list_t g_UsbDeviceLanguageList = {
    g_UsbDeviceString0,
    sizeof(g_UsbDeviceString0),
    g_UsbDeviceLanguage,
    USB_DEVICE_LANGUAGE_COUNT,
};

usb_status_t
USB_DeviceGetDeviceDescriptor(usb_device_handle handle,
                              usb_device_get_device_descriptor_struct_t *deviceDescriptor)
{
    deviceDescriptor->buffer = g_UsbDeviceDescriptor;
    deviceDescriptor->length = USB_DESCRIPTOR_LENGTH_DEVICE;
    return kStatus_USB_Success;
}

usb_status_t
USB_DeviceGetConfigurationDescriptor(usb_device_handle handle,
                                     usb_device_get_configuration_descriptor_struct_t *configurationDescriptor)
{
    if (USB_HID_GENERIC_CONFIGURE_INDEX > configurationDescriptor->configuration) {
        configurationDescriptor->buffer = g_UsbDeviceConfigurationDescriptor;
        configurationDescriptor->length = USB_DESCRIPTOR_LENGTH_CONFIGURATION_ALL;
        return kStatus_USB_Success;
    }
    return kStatus_USB_InvalidRequest;
}

usb_status_t
USB_DeviceGetStringDescriptor(usb_device_handle handle,
                              usb_device_get_string_descriptor_struct_t *stringDescriptor)
{
    if (stringDescriptor->stringIndex == 0) {
        stringDescriptor->buffer = (uint8_t *)g_UsbDeviceLanguageList.languageString;
        stringDescriptor->length = g_UsbDeviceLanguageList.stringLength;
    } else {
        uint8_t languageId = 0;
        uint8_t languageIndex = USB_DEVICE_STRING_COUNT;

        for (; languageId < USB_DEVICE_LANGUAGE_COUNT; languageId++) {
            if (stringDescriptor->languageId == g_UsbDeviceLanguageList.languageList[languageId].languageId) {
                if (stringDescriptor->stringIndex < USB_DEVICE_STRING_COUNT) {
                    languageIndex = stringDescriptor->stringIndex;
                }
                break;
            }
        }

        if (USB_DEVICE_STRING_COUNT == languageIndex) {
            return kStatus_USB_InvalidRequest;
        }
        stringDescriptor->buffer = (uint8_t *)g_UsbDeviceLanguageList.languageList[languageId].string[languageIndex];
        stringDescriptor->length = g_UsbDeviceLanguageList.languageList[languageId].length[languageIndex];
    }
    return kStatus_USB_Success;
}

usb_status_t
USB_DeviceGetHidDescriptor(usb_device_handle handle, usb_device_get_hid_descriptor_struct_t *hidDescriptor)
{
    return kStatus_USB_InvalidRequest;
}

usb_status_t
USB_DeviceGetHidReportDescriptor(usb_device_handle handle,
                                 usb_device_get_hid_report_descriptor_struct_t *hidReportDescriptor)
{
    if (USB_HID_GENERIC_INTERFACE_INDEX == hidReportDescriptor->interfaceNumber) {
        hidReportDescriptor->buffer = g_UsbDeviceHidGenericReportDescriptor;
        hidReportDescriptor->length = USB_DESCRIPTOR_LENGTH_HID_GENERIC_REPORT;
    } else {
        return kStatus_USB_InvalidRequest;
    }
    return kStatus_USB_Success;
}

usb_status_t
USB_DeviceGetHidPhysicalDescriptor(usb_device_handle handle,
                                   usb_device_get_hid_physical_descriptor_struct_t *hidPhysicalDescriptor)
{
    return kStatus_USB_InvalidRequest;
}

usb_dev_class_config_t g_UsbDeviceHidConfig[] = {
    {
        usb_dev_hid_cb,
        0,
        &g_UsbDeviceHidGenericConfig,
    },
};

static usb_device_class_config_list_struct_t g_UsbDeviceHidConfigList = {
    g_UsbDeviceHidConfig,
    usb_dev_cb,
    1,
};

static usb_status_t
usb_dev_hid_cb(class_handle_t handle, uint32_t event, void *param)
{
    usb_status_t error = kStatus_USB_Error;

    switch (event) {
    case kUSB_DeviceHidEventSendResponse:
        break;
    case kUSB_DeviceHidEventRecvResponse:
        if (g_UsbDeviceHidGeneric.attach) {
            usb_dev_hid_send(g_UsbDeviceHidGeneric.hidHandle, USB_HID_GENERIC_ENDPOINT_IN,
                             (uint8_t *)&g_UsbDeviceHidGeneric.buffer[g_UsbDeviceHidGeneric.bufferIndex][0],
                             USB_HID_GENERIC_OUT_BUFFER_LENGTH);
            g_UsbDeviceHidGeneric.bufferIndex ^= 1;
            return usb_dev_hid_recv(g_UsbDeviceHidGeneric.hidHandle, USB_HID_GENERIC_ENDPOINT_OUT,
                                    (uint8_t *)&g_UsbDeviceHidGeneric.buffer[g_UsbDeviceHidGeneric.bufferIndex][0],
                                    USB_HID_GENERIC_OUT_BUFFER_LENGTH);
        }
        break;
    case kUSB_DeviceHidEventGetReport:
    case kUSB_DeviceHidEventSetReport:
    case kUSB_DeviceHidEventRequestReportBuffer:
        error = kStatus_USB_InvalidRequest;
        break;
    case kUSB_DeviceHidEventGetIdle:
    case kUSB_DeviceHidEventGetProtocol:
    case kUSB_DeviceHidEventSetIdle:
    case kUSB_DeviceHidEventSetProtocol:
        break;
    default:
        break;
    }

    return error;
}

static usb_status_t
usb_dev_cb(usb_device_handle handle, uint32_t event, void *param)
{
    usb_status_t error = kStatus_USB_Success;
    uint8_t *temp8 = (uint8_t *)param;
    uint16_t *temp16 = (uint16_t *)param;

    switch (event)
    {
    case kUSB_DeviceEventBusReset:
        g_UsbDeviceHidGeneric.attach = 0;
        break;
    case kUSB_DeviceEventSetConfiguration:
        if (param) {
            g_UsbDeviceHidGeneric.attach = 1;
            g_UsbDeviceHidGeneric.currentConfiguration = *temp8;
            if (USB_HID_GENERIC_CONFIGURE_INDEX == (*temp8)) {
                error = usb_dev_hid_recv(
                    g_UsbDeviceHidGeneric.hidHandle, USB_HID_GENERIC_ENDPOINT_OUT,
                    (uint8_t *)&g_UsbDeviceHidGeneric.buffer[g_UsbDeviceHidGeneric.bufferIndex][0],
                    USB_HID_GENERIC_OUT_BUFFER_LENGTH);
            }
        }
        break;
    case kUSB_DeviceEventSetInterface:
        if (g_UsbDeviceHidGeneric.attach) {
            uint8_t interface = (uint8_t)((*temp16 & 0xFF00) >> 0x08);
            uint8_t alternateSetting = (uint8_t)(*temp16 & 0x00FF);
            if (interface < USB_HID_GENERIC_INTERFACE_COUNT) {
                g_UsbDeviceHidGeneric.currentInterfaceAlternateSetting[interface] = alternateSetting;
                if (alternateSetting == 0) {
                    error = usb_dev_hid_recv(
                        g_UsbDeviceHidGeneric.hidHandle, USB_HID_GENERIC_ENDPOINT_OUT,
                        (uint8_t *)&g_UsbDeviceHidGeneric.buffer[g_UsbDeviceHidGeneric.bufferIndex][0],
                        USB_HID_GENERIC_OUT_BUFFER_LENGTH);
                }
            }
        }
        break;
    case kUSB_DeviceEventGetConfiguration:
        if (param) {
            *temp8 = g_UsbDeviceHidGeneric.currentConfiguration;
            error = kStatus_USB_Success;
        }
        break;
    case kUSB_DeviceEventGetInterface:
        if (param) {
            uint8_t interface = (uint8_t)((*temp16 & 0xFF00) >> 0x08);
            if (interface < USB_HID_GENERIC_INTERFACE_COUNT) {
                *temp16 = (*temp16 & 0xFF00) | g_UsbDeviceHidGeneric.currentInterfaceAlternateSetting[interface];
                error = kStatus_USB_Success;
            } else {
                error = kStatus_USB_InvalidRequest;
            }
        }
        break;
    case kUSB_DeviceEventGetDeviceDescriptor:
        if (param) {
            error = USB_DeviceGetDeviceDescriptor(handle, (usb_device_get_device_descriptor_struct_t *)param);
        }
        break;
    case kUSB_DeviceEventGetConfigurationDescriptor:
        if (param) {
            error = USB_DeviceGetConfigurationDescriptor(handle,
                                                         (usb_device_get_configuration_descriptor_struct_t *)param);
        }
        break;
    case kUSB_DeviceEventGetStringDescriptor:
        if (param) {
            error = USB_DeviceGetStringDescriptor(handle, (usb_device_get_string_descriptor_struct_t *)param);
        }
        break;
    case kUSB_DeviceEventGetHidDescriptor:
        if (param) {
            error = USB_DeviceGetHidDescriptor(handle, (usb_device_get_hid_descriptor_struct_t *)param);
        }
        break;
    case kUSB_DeviceEventGetHidReportDescriptor:
        if (param) {
            error =
                USB_DeviceGetHidReportDescriptor(handle, (usb_device_get_hid_report_descriptor_struct_t *)param);
        }
        break;
    case kUSB_DeviceEventGetHidPhysicalDescriptor:
        if (param) {
            error = USB_DeviceGetHidPhysicalDescriptor(handle,
                                                       (usb_device_get_hid_physical_descriptor_struct_t *)param);
        }
        break;
    default:
        break;
    }

    return error;
}

static void
usb_device_application_init(void)
{
    usb_status_t status;

    usb_hal_init_clocks();
    usb_hal_clear_memory();

    g_UsbDeviceHidGeneric.speed = USB_SPEED_FULL;
    g_UsbDeviceHidGeneric.attach = 0;
    g_UsbDeviceHidGeneric.hidHandle = (class_handle_t)NULL;
    g_UsbDeviceHidGeneric.deviceHandle = NULL;
    g_UsbDeviceHidGeneric.buffer[0] = (uint8_t *)&s_GenericBuffer0[0];
    g_UsbDeviceHidGeneric.buffer[1] = (uint8_t *)&s_GenericBuffer1[0];

    status = usb_device_class_init(CONTROLLER_ID, &g_UsbDeviceHidConfigList, &g_UsbDeviceHidGeneric.deviceHandle);
    if (status == kStatus_USB_Success) {
        g_UsbDeviceHidGeneric.hidHandle = g_UsbDeviceHidConfigList.config->classHandle;
    } else {
        return;
    }

    usb_hal_set_dev_handle(g_UsbDeviceHidGeneric.deviceHandle);
    usb_hal_enable_irq();
    usb_device_run(g_UsbDeviceHidGeneric.deviceHandle);
}

static void
usb_dev_task_handler(void *handle)
{
    while (1) {
        usb_device_task_fn(handle);
        //os_time_delay(1);
    }
}

static void
blinker_handler(void *arg)
{
    hal_gpio_init_out(LED_BLINK_PIN, 1);

    while (1) {
        os_time_delay(OS_TICKS_PER_SEC * 3);
        hal_gpio_toggle(LED_BLINK_PIN);
    }
}

static void
init_tasks(void)
{
    os_stack_t *pstack;

    pstack = malloc(sizeof(os_stack_t) * BLINKER_STACK_SIZE);
    assert(pstack);

    os_task_init(&blinker_task, "led_blinker", blinker_handler, NULL,
            BLINKER_PRIO, OS_WAIT_FOREVER, pstack, BLINKER_STACK_SIZE);

    usb_device_application_init();

    pstack = malloc(sizeof(os_stack_t) * USB_DEV_STACK_SIZE);
    assert(pstack);

    os_task_init(&usb_dev_task, "usb_dev_task", usb_dev_task_handler, g_UsbDeviceHidGeneric.deviceHandle,
            USB_DEV_TASK_PRIO, OS_WAIT_FOREVER, pstack, USB_DEV_STACK_SIZE);
}

int
main(int argc, char **argv)
{
    //int rc;

#ifdef ARCH_sim
    mcu_sim_parse_args(argc, argv);
#endif

    sysinit();

    console_printf("booting\n");

    init_tasks();

    /*
     * As the last thing, process events from default event queue.
     */
    while (1) {
        os_eventq_run(os_eventq_dflt_get());
    }
}
