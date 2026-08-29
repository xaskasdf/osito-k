/*
 * SDL dynamically probes libusb even when native HID input is available.
 * OsitoK owns USB devices in its xHCI driver, so expose an empty libusb bus
 * instead of letting the Windows libusb backend probe unsupported device APIs.
 */

#include "libusb_shim.h"

extern void WINAPI Sleep(DWORD milliseconds);

#define LIBUSB_ERROR_NO_DEVICE     (-4)
#define LIBUSB_ERROR_NOT_SUPPORTED (-12)

static ULONG_PTR libusb_context;
static PVOID libusb_empty_devices[1];

static int WINAPI usb_init(PVOID *context)
{
    if (context) *context = &libusb_context;
    return 0;
}

static void WINAPI usb_exit(PVOID context)
{
    (void)context;
}

static LONG_PTR WINAPI usb_get_device_list(PVOID context, PVOID **devices)
{
    (void)context;
    if (devices) *devices = libusb_empty_devices;
    return 0;
}

static void WINAPI usb_free_device_list(PVOID *devices, int unref_devices)
{
    (void)devices;
    (void)unref_devices;
}

static int WINAPI usb_device_error_2(PVOID device, PVOID output)
{
    (void)device;
    (void)output;
    return LIBUSB_ERROR_NO_DEVICE;
}

static int WINAPI usb_get_config_descriptor(PVOID device, BYTE index,
                                             PVOID output)
{
    (void)device;
    (void)index;
    (void)output;
    return LIBUSB_ERROR_NO_DEVICE;
}

static void WINAPI usb_free_config_descriptor(PVOID descriptor)
{
    (void)descriptor;
}

static BYTE WINAPI usb_device_byte(PVOID device)
{
    (void)device;
    return 0;
}

static int WINAPI usb_get_port_numbers(PVOID device, BYTE *ports, BYTE length)
{
    (void)device;
    (void)ports;
    (void)length;
    return LIBUSB_ERROR_NO_DEVICE;
}

static int WINAPI usb_open(PVOID device, PVOID *handle)
{
    (void)device;
    if (handle) *handle = NULL;
    return LIBUSB_ERROR_NO_DEVICE;
}

static void WINAPI usb_close(PVOID handle)
{
    (void)handle;
}

static PVOID WINAPI usb_get_device(PVOID handle)
{
    (void)handle;
    return NULL;
}

static int WINAPI usb_handle_index(PVOID handle, int index)
{
    (void)handle;
    (void)index;
    return LIBUSB_ERROR_NO_DEVICE;
}

static int WINAPI usb_set_auto_detach(PVOID handle, int enable)
{
    (void)handle;
    (void)enable;
    return 0;
}

static int WINAPI usb_set_alt(PVOID handle, int interface_number,
                              int alternate_setting)
{
    (void)handle;
    (void)interface_number;
    (void)alternate_setting;
    return LIBUSB_ERROR_NO_DEVICE;
}

static PVOID WINAPI usb_alloc_transfer(int iso_packets)
{
    (void)iso_packets;
    return NULL;
}

static int WINAPI usb_transfer_error(PVOID transfer)
{
    (void)transfer;
    return LIBUSB_ERROR_NO_DEVICE;
}

static void WINAPI usb_free_transfer(PVOID transfer)
{
    (void)transfer;
}

static int WINAPI usb_control_transfer(PVOID handle, BYTE request_type,
                                       BYTE request, WORD value, WORD index,
                                       BYTE *data, WORD length, UINT timeout)
{
    (void)handle;
    (void)request_type;
    (void)request;
    (void)value;
    (void)index;
    (void)data;
    (void)length;
    (void)timeout;
    return LIBUSB_ERROR_NO_DEVICE;
}

static int WINAPI usb_bulk_transfer(PVOID handle, BYTE endpoint, BYTE *data,
                                    int length, int *transferred, UINT timeout)
{
    (void)handle;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)timeout;
    if (transferred) *transferred = 0;
    return LIBUSB_ERROR_NO_DEVICE;
}

static int WINAPI usb_handle_events(PVOID context)
{
    (void)context;
    Sleep(25);
    return 0;
}

static int WINAPI usb_handle_events_completed(PVOID context, int *completed)
{
    if (completed && *completed) return 0;
    return usb_handle_events(context);
}

static void WINAPI usb_interrupt_event_handler(PVOID context)
{
    (void)context;
}

static int WINAPI usb_has_capability(UINT capability)
{
    (void)capability;
    return 0;
}

static int WINAPI usb_hotplug_register(PVOID context, int events, int flags,
                                       int vendor, int product, int dev_class,
                                       PVOID callback, PVOID user_data,
                                       int *callback_handle)
{
    (void)context;
    (void)events;
    (void)flags;
    (void)vendor;
    (void)product;
    (void)dev_class;
    (void)callback;
    (void)user_data;
    if (callback_handle) *callback_handle = 0;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

static void WINAPI usb_hotplug_deregister(PVOID context, int callback_handle)
{
    (void)context;
    (void)callback_handle;
}

static PCSTR WINAPI usb_error_name(int error)
{
    if (error == LIBUSB_ERROR_NO_DEVICE) return "LIBUSB_ERROR_NO_DEVICE";
    if (error == LIBUSB_ERROR_NOT_SUPPORTED)
        return "LIBUSB_ERROR_NOT_SUPPORTED";
    return error == 0 ? "LIBUSB_SUCCESS" : "LIBUSB_ERROR_OTHER";
}

static const WIN32_EXPORT libusb_exports[] = {
    { "libusb_init", (PVOID)usb_init, 1, CC_CDECL },
    { "libusb_exit", (PVOID)usb_exit, 1, CC_CDECL },
    { "libusb_get_device_list", (PVOID)usb_get_device_list, 2, CC_CDECL },
    { "libusb_free_device_list", (PVOID)usb_free_device_list, 2, CC_CDECL },
    { "libusb_get_device_descriptor", (PVOID)usb_device_error_2, 2, CC_CDECL },
    { "libusb_get_active_config_descriptor", (PVOID)usb_device_error_2, 2, CC_CDECL },
    { "libusb_get_config_descriptor", (PVOID)usb_get_config_descriptor, 3, CC_CDECL },
    { "libusb_free_config_descriptor", (PVOID)usb_free_config_descriptor, 1, CC_CDECL },
    { "libusb_get_bus_number", (PVOID)usb_device_byte, 1, CC_CDECL },
    { "libusb_get_port_numbers", (PVOID)usb_get_port_numbers, 3, CC_CDECL },
    { "libusb_get_device_address", (PVOID)usb_device_byte, 1, CC_CDECL },
    { "libusb_open", (PVOID)usb_open, 2, CC_CDECL },
    { "libusb_close", (PVOID)usb_close, 1, CC_CDECL },
    { "libusb_get_device", (PVOID)usb_get_device, 1, CC_CDECL },
    { "libusb_claim_interface", (PVOID)usb_handle_index, 2, CC_CDECL },
    { "libusb_release_interface", (PVOID)usb_handle_index, 2, CC_CDECL },
    { "libusb_kernel_driver_active", (PVOID)usb_handle_index, 2, CC_CDECL },
    { "libusb_detach_kernel_driver", (PVOID)usb_handle_index, 2, CC_CDECL },
    { "libusb_attach_kernel_driver", (PVOID)usb_handle_index, 2, CC_CDECL },
    { "libusb_set_auto_detach_kernel_driver", (PVOID)usb_set_auto_detach, 2, CC_CDECL },
    { "libusb_set_interface_alt_setting", (PVOID)usb_set_alt, 3, CC_CDECL },
    { "libusb_alloc_transfer", (PVOID)usb_alloc_transfer, 1, CC_CDECL },
    { "libusb_submit_transfer", (PVOID)usb_transfer_error, 1, CC_CDECL },
    { "libusb_cancel_transfer", (PVOID)usb_transfer_error, 1, CC_CDECL },
    { "libusb_free_transfer", (PVOID)usb_free_transfer, 1, CC_CDECL },
    { "libusb_control_transfer", (PVOID)usb_control_transfer, 8, CC_CDECL },
    { "libusb_interrupt_transfer", (PVOID)usb_bulk_transfer, 6, CC_CDECL },
    { "libusb_bulk_transfer", (PVOID)usb_bulk_transfer, 6, CC_CDECL },
    { "libusb_handle_events", (PVOID)usb_handle_events, 1, CC_CDECL },
    { "libusb_handle_events_completed", (PVOID)usb_handle_events_completed, 2, CC_CDECL },
    { "libusb_interrupt_event_handler", (PVOID)usb_interrupt_event_handler, 1, CC_CDECL },
    { "libusb_has_capability", (PVOID)usb_has_capability, 1, CC_CDECL },
    { "libusb_hotplug_register_callback", (PVOID)usb_hotplug_register, 9, CC_CDECL },
    { "libusb_hotplug_deregister_callback", (PVOID)usb_hotplug_deregister, 2, CC_CDECL },
    { "libusb_error_name", (PVOID)usb_error_name, 1, CC_CDECL },
    { NULL, NULL, 0, CC_CDECL }
};

static int usb_strcmp(const char *left, const char *right)
{
    while (*left && *right && *left == *right) { left++; right++; }
    return (unsigned char)*left - (unsigned char)*right;
}

PVOID libusb_resolve(const char *func_name, USHORT ordinal, BOOL by_ordinal)
{
    (void)ordinal;
    if (by_ordinal || !func_name) return NULL;
    for (int i = 0; libusb_exports[i].name; i++) {
        if (usb_strcmp(func_name, libusb_exports[i].name) == 0)
            return libusb_exports[i].func;
    }
    return NULL;
}

const WIN32_EXPORT *libusb_abi_table(int *count)
{
    *count = (int)(sizeof(libusb_exports) / sizeof(libusb_exports[0]));
    return libusb_exports;
}

void libusb_shim_init(void)
{
    libusb_context = 0;
    libusb_empty_devices[0] = NULL;
}
