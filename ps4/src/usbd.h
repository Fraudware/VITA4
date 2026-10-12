// VITA4 - PS4 libSceUsbd (libusb-1.0 style host API) declarations.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenOrbis ships orbis/Usbd.h but not the libusb.h it includes, so the few
// entry points VITA4 uses are declared here. Structs are kept opaque: raw
// descriptors are fetched with sceUsbdGetDescriptor and parsed by hand, and
// transfer structs go through xfer_layout.h.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;
typedef struct libusb_transfer libusb_transfer;
typedef void (*vd_usbd_transfer_cb)(libusb_transfer *transfer);

// Standard 18-byte USB device descriptor (natural alignment = wire layout).
typedef struct
{
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} VdUsbDeviceDescriptor;

int32_t sceUsbdInit(void);
int32_t sceUsbdExit(void);
int32_t sceUsbdGetDeviceList(libusb_device ***list);
int32_t sceUsbdFreeDeviceList(libusb_device **list);
int32_t sceUsbdGetDeviceDescriptor(libusb_device *dev, VdUsbDeviceDescriptor *desc);
int32_t sceUsbdOpen(libusb_device *dev, libusb_device_handle **handle);
void sceUsbdClose(libusb_device_handle *handle);
libusb_device_handle *sceUsbdOpenDeviceWithVidPid(uint16_t vendor_id, uint16_t product_id);
int32_t sceUsbdGetDescriptor(libusb_device_handle *h, uint8_t desc_type, uint8_t desc_index,
                             unsigned char *data, int32_t length);
int32_t sceUsbdKernelDriverActive(libusb_device_handle *h, int32_t interface_number);
int32_t sceUsbdDetachKernelDriver(libusb_device_handle *h, int32_t interface_number);
int32_t sceUsbdClaimInterface(libusb_device_handle *h, int32_t interface_number);
int32_t sceUsbdReleaseInterface(libusb_device_handle *h, int32_t interface_number);
int32_t sceUsbdSetInterfaceAltSetting(libusb_device_handle *h, int32_t interface_number,
                                      int32_t alternate_setting);
int32_t sceUsbdClearHalt(libusb_device_handle *h, unsigned char endpoint);
int32_t sceUsbdControlTransfer(libusb_device_handle *h, uint8_t bmRequestType, uint8_t bRequest,
                               uint16_t wValue, uint16_t wIndex, unsigned char *data,
                               uint16_t wLength, uint32_t timeout);
int32_t sceUsbdBulkTransfer(libusb_device_handle *h, unsigned char endpoint, unsigned char *data,
                            int32_t length, int32_t *transferred, uint32_t timeout);
libusb_transfer *sceUsbdAllocTransfer(int32_t iso_packets);
void sceUsbdFreeTransfer(libusb_transfer *transfer);
int32_t sceUsbdSubmitTransfer(libusb_transfer *transfer);
int32_t sceUsbdCancelTransfer(libusb_transfer *transfer);
int32_t sceUsbdHandleEvents(void);

#ifdef __cplusplus
}
#endif

// libusb error codes. libSceUsbd may report them either as plain negative
// libusb codes or as SCE codes 0x8024xxxx; vd_usbd_err() folds both to the
// libusb form.
enum
{
    VD_USB_OK = 0,
    VD_USB_ERR_IO = -1,
    VD_USB_ERR_INVALID = -2,
    VD_USB_ERR_ACCESS = -3,
    VD_USB_ERR_NO_DEVICE = -4,
    VD_USB_ERR_NOT_FOUND = -5,
    VD_USB_ERR_BUSY = -6,
    VD_USB_ERR_TIMEOUT = -7,
    VD_USB_ERR_OVERFLOW = -8,
    VD_USB_ERR_PIPE = -9,
    VD_USB_ERR_INTERRUPTED = -10,
    VD_USB_ERR_NO_MEM = -11,
    VD_USB_ERR_NOT_SUPPORTED = -12,
};

static inline int vd_usbd_err(int32_t rc)
{
    if (rc >= 0)
        return rc;
    if (rc > -100)
        return rc;
    const uint32_t u = (uint32_t)rc;
    if ((u & 0xFFFFFF00u) == 0x80240000u)
    {
        int code = (int)(u & 0xFFu);
        return code >= 1 && code <= 12 ? -code : VD_USB_ERR_IO;
    }
    return VD_USB_ERR_IO;
}
