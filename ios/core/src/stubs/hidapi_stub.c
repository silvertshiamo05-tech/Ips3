// hidapi for iOS: no devices, ever.
//
// iOS apps cannot reach IOKit HID or raw USB, so there is nothing for hidapi to talk to.
// RPCS3's DS3/DS4/DualSense/PS Move handlers are still compiled (pad_thread references them
// directly), and this lets them initialise, enumerate an empty list and stay idle. Real
// controllers reach the emulator through GameController.framework and the virtual pad.

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "hidapi.h"
#include "hidapi_darwin.h"

static const wchar_t k_no_hid[] = L"HID devices are not available on iOS";

int HID_API_EXPORT HID_API_CALL hid_init(void) { return 0; }
int HID_API_EXPORT HID_API_CALL hid_exit(void) { return 0; }

struct hid_device_info HID_API_EXPORT* HID_API_CALL hid_enumerate(unsigned short vendor_id, unsigned short product_id)
{
	(void)vendor_id;
	(void)product_id;
	return NULL;
}

void HID_API_EXPORT HID_API_CALL hid_free_enumeration(struct hid_device_info* devs) { (void)devs; }

HID_API_EXPORT hid_device* HID_API_CALL hid_open(unsigned short vendor_id, unsigned short product_id, const wchar_t* serial_number)
{
	(void)vendor_id;
	(void)product_id;
	(void)serial_number;
	return NULL;
}

HID_API_EXPORT hid_device* HID_API_CALL hid_open_path(const char* path)
{
	(void)path;
	return NULL;
}

int HID_API_EXPORT HID_API_CALL hid_write(hid_device* dev, const unsigned char* data, size_t length)
{
	(void)dev; (void)data; (void)length;
	return -1;
}

int HID_API_EXPORT HID_API_CALL hid_read_timeout(hid_device* dev, unsigned char* data, size_t length, int milliseconds)
{
	(void)dev; (void)data; (void)length; (void)milliseconds;
	return -1;
}

int HID_API_EXPORT HID_API_CALL hid_read(hid_device* dev, unsigned char* data, size_t length)
{
	return hid_read_timeout(dev, data, length, 0);
}

HID_API_EXPORT const wchar_t* HID_API_CALL hid_read_error(hid_device* dev)
{
	(void)dev;
	return k_no_hid;
}

int HID_API_EXPORT HID_API_CALL hid_set_nonblocking(hid_device* dev, int nonblock)
{
	(void)dev; (void)nonblock;
	return -1;
}

int HID_API_EXPORT HID_API_CALL hid_send_feature_report(hid_device* dev, const unsigned char* data, size_t length)
{
	(void)dev; (void)data; (void)length;
	return -1;
}

int HID_API_EXPORT HID_API_CALL hid_get_feature_report(hid_device* dev, unsigned char* data, size_t length)
{
	(void)dev; (void)data; (void)length;
	return -1;
}

int HID_API_EXPORT HID_API_CALL hid_send_output_report(hid_device* dev, const unsigned char* data, size_t length)
{
	(void)dev; (void)data; (void)length;
	return -1;
}

int HID_API_EXPORT HID_API_CALL hid_get_input_report(hid_device* dev, unsigned char* data, size_t length)
{
	(void)dev; (void)data; (void)length;
	return -1;
}

void HID_API_EXPORT HID_API_CALL hid_close(hid_device* dev) { (void)dev; }

int HID_API_EXPORT_CALL hid_get_manufacturer_string(hid_device* dev, wchar_t* string, size_t maxlen)
{
	(void)dev; (void)string; (void)maxlen;
	return -1;
}

int HID_API_EXPORT_CALL hid_get_product_string(hid_device* dev, wchar_t* string, size_t maxlen)
{
	(void)dev; (void)string; (void)maxlen;
	return -1;
}

int HID_API_EXPORT_CALL hid_get_serial_number_string(hid_device* dev, wchar_t* string, size_t maxlen)
{
	(void)dev; (void)string; (void)maxlen;
	return -1;
}

struct hid_device_info HID_API_EXPORT* HID_API_CALL hid_get_device_info(hid_device* dev)
{
	(void)dev;
	return NULL;
}

int HID_API_EXPORT_CALL hid_get_indexed_string(hid_device* dev, int string_index, wchar_t* string, size_t maxlen)
{
	(void)dev; (void)string_index; (void)string; (void)maxlen;
	return -1;
}

int HID_API_EXPORT_CALL hid_get_report_descriptor(hid_device* dev, unsigned char* buf, size_t buf_size)
{
	(void)dev; (void)buf; (void)buf_size;
	return -1;
}

HID_API_EXPORT const wchar_t* HID_API_CALL hid_error(hid_device* dev)
{
	(void)dev;
	return k_no_hid;
}

static const struct hid_api_version k_version = {HID_API_VERSION_MAJOR, HID_API_VERSION_MINOR, HID_API_VERSION_PATCH};

HID_API_EXPORT const struct hid_api_version* HID_API_CALL hid_version(void) { return &k_version; }
HID_API_EXPORT const char* HID_API_CALL hid_version_str(void) { return HID_API_VERSION_STR; }

int HID_API_EXPORT_CALL hid_darwin_get_location_id(hid_device* dev, uint32_t* location_id)
{
	(void)dev; (void)location_id;
	return -1;
}

void HID_API_EXPORT_CALL hid_darwin_set_open_exclusive(int open_exclusive) { (void)open_exclusive; }
int HID_API_EXPORT_CALL hid_darwin_get_open_exclusive(void) { return 0; }

int HID_API_EXPORT_CALL hid_darwin_is_device_open_exclusive(hid_device* dev)
{
	(void)dev;
	return -1;
}
