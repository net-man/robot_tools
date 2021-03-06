#include "debug.h"
#include "rhid.h"

#include <corecrt_malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vcruntime_string.h>
#include <windows.h>
#include <errhandlingapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <SetupAPI.h>

// TODO Handle HID disconnection gracefully.
// TODO Rename size to count in places where it refers to an array size to avoid
// confusion.

typedef unsigned long  ulong;
typedef unsigned short ushort;

#ifdef RHID_DEBUG_ENABLED
#ifndef DEBUG_TIME
#define DEBUG_TIME
#endif

#define RHID_VARGS(...) __VA_ARGS__
#define RHID_ERR(message, ...) fprintf(stderr, message "\n", __VA_ARGS__);
#define RHID_ERR_SYS(message, sys_err)                                  \
	{                                                                   \
		char errmsg[256];                                               \
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, sys_err,        \
					  LANG_USER_DEFAULT, errmsg, sizeof(errmsg), NULL); \
		fprintf(stderr, message);                                       \
		fprintf(stderr, " error(%#010x): %s\n", sys_err, errmsg);       \
	}
#else
#define RHID_ERR(message)
#define RHID_ERR_SYS(message, sys_err)
#define RHID_VARGS(...)
#endif

#define ERROR_TO_STRING_CASE(char_msg, err) \
	case(err):                              \
		(char_msg) = #err;                  \
		break
static const char* _rhid_hidp_err_to_str(ulong status) {
	const char* msg;

	switch(status) {
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_NULL);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_INVALID_PREPARSED_DATA);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_INVALID_REPORT_TYPE);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_INVALID_REPORT_LENGTH);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_USAGE_NOT_FOUND);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_VALUE_OUT_OF_RANGE);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_BAD_LOG_PHY_VALUES);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_BUFFER_TOO_SMALL);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_INTERNAL_ERROR);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_I8042_TRANS_UNKNOWN);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_INCOMPATIBLE_REPORT_ID);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_NOT_VALUE_ARRAY);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_IS_VALUE_ARRAY);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_DATA_INDEX_NOT_FOUND);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_DATA_INDEX_OUT_OF_RANGE);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_BUTTON_NOT_PRESSED);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_REPORT_DOES_NOT_EXIST);
		ERROR_TO_STRING_CASE(msg, HIDP_STATUS_NOT_IMPLEMENTED);
		default:
			return "NOT_A_HIDP_ERROR";
	}

	return msg;
}

// TODO remove _rhid_win_gcache as most of what I thought it was needed for in
// largely unessasery.
static struct {
	ulong dev_iface_list_size;
	char* dev_iface_list;

	int				  button_caps_count;
	HIDP_BUTTON_CAPS* button_caps;

	int				 value_caps_count;
	HIDP_VALUE_CAPS* value_caps;

	ulong			usages_pages_count;
	USAGE_AND_PAGE* usages_pages;
	USAGE*			usages_ordered;

	// TODO move report_overlapped to device so more than one device can be
	// opened at once.
	OVERLAPPED report_overlapped;
} _rhid_win_gcache = {0};

struct rhid_native_t {
	int		   is_reading;
	OVERLAPPED report_overlapped;
};

// TODO rhid_get_device_count can be optimized.
// to do this, store a pre-allocated buffer for the interface list in a
// global cache. then, re-allocate it when
// CM_Get_Device_Interface_List_SizeA is greater than the global cache
// size. this way, memory isn't allocated and free every time
// rhid_get_device_count is called.

int rhid_get_device_count() {
	// get the HIDClass devices guid.
	GUID hid_guid = {0};
	HidD_GetHidGuid(&hid_guid);

	// get a list of devices from the devices class.
	HDEVINFO dev_list = SetupDiGetClassDevsA(
		&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

	if(dev_list == INVALID_HANDLE_VALUE) {
		RHID_ERR_SYS("failed to get devices from device class", GetLastError());
		return -1;
	}

	// jump through the enumeration until we get out of bounds.
	const int jump_count = 5;
	int		  last_index = 0;

	for(int i = jump_count;; i += jump_count) {
		SP_DEVICE_INTERFACE_DATA iface = {0};
		iface.cbSize				   = sizeof(SP_DEVICE_INTERFACE_DATA);
		if(SetupDiEnumDeviceInterfaces(dev_list, NULL, &hid_guid, i, &iface) ==
		   FALSE) {
			if(GetLastError() == ERROR_NO_MORE_ITEMS) {
				// we are too far outside of the array. break out and decriment
				// i until we have reached the last valid index.
				last_index = i;
				break;
			}
			else {
				RHID_ERR_SYS("failed to enumerate through device interfaces",
							 GetLastError());
				SetupDiDestroyDeviceInfoList(dev_list);
				return -1;
			}
		}
	}

	// back-track to the last valid index which will be the final size.
	for(; last_index >= 0; last_index--) {
		SP_DEVICE_INTERFACE_DATA iface = {0};
		iface.cbSize				   = sizeof(SP_DEVICE_INTERFACE_DATA);
		if(SetupDiEnumDeviceInterfaces(dev_list, NULL, &hid_guid, last_index,
									   &iface) == TRUE) {
			break;
		}
	}

	// clean-up the device list.
	SetupDiDestroyDeviceInfoList(dev_list);

	// return the final count.
	return last_index + 1;
}

static inline void* _rhid_open_device_handle(const char* path,
											 ulong		 access_rights,
											 ulong		 share_mode) {
	void* handle = CreateFileA(path, access_rights, share_mode, NULL,
							   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if(handle == INVALID_HANDLE_VALUE) {
		RHID_ERR_SYS(RHID_VARGS("failed to open device \"%s\"", path),
					 GetLastError());
		return NULL;
	}

	return handle;
}

// TODO reduce the insane level of nested loops and if statements.
//		I think there is something like 7 levels of nesting at one point.
int rhid_get_devices(rhid_device_t* devices, int count) {
	// get the HIDClass devices guid.
	GUID hid_guid = {0};
	HidD_GetHidGuid(&hid_guid);

	// get a list of devices from the devices class.
	HDEVINFO dev_list = SetupDiGetClassDevsA(
		&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

	if(dev_list == INVALID_HANDLE_VALUE) {
		RHID_ERR_SYS("failed to get devices from device class", GetLastError());
		return -1;
	}

	for(int i = 0; i < count; i++) {
		// set device values to zero.
		memset(&devices[i], 0, sizeof(rhid_device_t));

		// get the next interface at index i. All the device interface is for is
		// getting the device path.
		SP_DEVICE_INTERFACE_DATA iface = {0};
		iface.cbSize				   = sizeof(SP_DEVICE_INTERFACE_DATA);

		if(SetupDiEnumDeviceInterfaces(dev_list, NULL, &hid_guid, i, &iface) ==
		   FALSE) {
			if(GetLastError() == ERROR_NO_MORE_ITEMS) {
				break;
			}
			else {
				RHID_ERR_SYS("failed to enumerate through device interfaces",
							 GetLastError());
				SetupDiDestroyDeviceInfoList(dev_list);
				return -1;
			}
		}

		// get the interface detail size.
		ulong iface_info_size = 0;
		if(SetupDiGetDeviceInterfaceDetailA(dev_list, &iface, NULL, 0,
											&iface_info_size, NULL) == FALSE) {
			if(GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
				RHID_ERR_SYS("failed to get device interface detail size",
							 GetLastError());
				return -1;
			}
		}

		// get the device interface details.
		SP_DEVICE_INTERFACE_DETAIL_DATA_A* iface_info =
			malloc(iface_info_size * sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A));
		iface_info->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

		if(SetupDiGetDeviceInterfaceDetailA(dev_list, &iface, iface_info,
											iface_info_size, NULL,
											NULL) == FALSE) {
			RHID_ERR_SYS("failed to get device interface detail data",
						 GetLastError());
			return -1;
		}

		// copy over the device path.
		int next_iface_size = strlen(iface_info->DevicePath) + 1;
		memcpy(devices[i].path, iface_info->DevicePath, next_iface_size);

		if(devices[i].path[next_iface_size - 4] == 'k' &&
		   devices[i].path[next_iface_size - 3] == 'b' &&
		   devices[i].path[next_iface_size - 2] == 'd') {
			memset(devices[i].path + next_iface_size - 5, 0, 5);
		}

		free(iface_info);
		RHID_ERR("\ngetting device (%i) \"%s\"", i, devices[i].path);
		// open the the device with as little permissions as possible so we can
		// read some attributes.
		devices[i].handle =
			_rhid_open_device_handle(devices[i].path, MAXIMUM_ALLOWED,
									 FILE_SHARE_READ | FILE_SHARE_WRITE);
		if(devices[i].handle == NULL) {
			devices[i].handle = _rhid_open_device_handle(
				devices[i].path, MAXIMUM_ALLOWED, FILE_SHARE_READ);

			if(devices[i].handle == NULL) {
				continue;
			}
		}

		// get general attributes of the device.
		HIDD_ATTRIBUTES attributes = {0};
		if(HidD_GetAttributes(devices[i].handle, &attributes) == TRUE) {
			devices[i].vendor_id  = attributes.VendorID;
			devices[i].product_id = attributes.ProductID;
			devices[i].version	  = attributes.VersionNumber;
		}
		else {
			RHID_ERR("faild to retrieve device attributes");
		}

		// TODO think about moving manufacturer_name to global cache.
		// this could potentially optimize the allocation and deletion of the
		// 254 bytes that make up this 2-wide string.

		// get the manufacturer of the device.
		wchar_t manufacturer_name[127];
		if(HidD_GetManufacturerString(devices[i].handle, manufacturer_name,
									  sizeof(manufacturer_name)) == TRUE) {
			wcstombs(devices[i].manufacturer_name, manufacturer_name,
					 sizeof(devices[i].manufacturer_name));
		}
		else {
			RHID_ERR("failed to retrieve device manufacturer name");
		}

		// TODO think about moving product_name to global cache.
		// this would be for the same reason as manufacturer_name.

		// get the product name of the device.
		wchar_t product_name[127];
		if(HidD_GetProductString(devices[i].handle, product_name,
								 sizeof(product_name)) == TRUE) {
			wcstombs(devices[i].product_name, product_name,
					 sizeof(devices[i].product_name));
		}
		else {
			RHID_ERR("failed to retrieve device product name");
		}

		// get preparsed data from the device.
		PHIDP_PREPARSED_DATA preparsed = {0};
		if(HidD_GetPreparsedData(devices[i].handle, &preparsed) == FALSE) {
			RHID_ERR("failed to get pre-parsed data from device");

			CloseHandle(devices[i].handle);
			devices[i].handle = NULL;
			continue;
		}

		// get device capabilities for various relatively useful attributes.
		// although this is probably the last HidP_GetCaps, HidP_GetButtonsCaps
		// and HidP_GetValueCaps will probably get called again when reading
		// input reports.
		HIDP_CAPS dev_caps;
		{
			ulong ret =
				HidP_GetCaps((PHIDP_PREPARSED_DATA) preparsed, &dev_caps);
			if(ret != HIDP_STATUS_SUCCESS) {
				RHID_ERR("failed to get device's capabilities error: %s",
						 _rhid_hidp_err_to_str(ret));

				CloseHandle(devices[i].handle);
				devices[i].handle = NULL;
				HidD_FreePreparsedData(preparsed);
				continue;
			}
		}

		// get button caps.
		if(dev_caps.NumberInputButtonCaps > 0) {
			if(_rhid_win_gcache.button_caps_count <
			   dev_caps.NumberInputButtonCaps) {
				if(_rhid_win_gcache.button_caps == NULL) {
					_rhid_win_gcache.button_caps =
						malloc(sizeof(HIDP_BUTTON_CAPS) *
							   dev_caps.NumberInputButtonCaps);
				}
				else {
					_rhid_win_gcache.button_caps =
						realloc(_rhid_win_gcache.button_caps,
								sizeof(HIDP_BUTTON_CAPS) *
									dev_caps.NumberInputButtonCaps);
				}

				_rhid_win_gcache.button_caps_count =
					dev_caps.NumberInputButtonCaps;
			}
			HIDP_BUTTON_CAPS* button_caps = _rhid_win_gcache.button_caps;
			devices[i].cap_button_count	  = dev_caps.NumberInputButtonCaps;
			{
				ulong ret = HidP_GetButtonCaps(
					HidP_Input, button_caps,
					(PUSHORT) &devices[i].cap_button_count, preparsed);
				if(ret != HIDP_STATUS_SUCCESS) {
					RHID_ERR("failed to get device's button error: %s",
							 _rhid_hidp_err_to_str(ret));
				}
				else {
					// assign button report ids, page, usage, and index.
					if(dev_caps.NumberInputButtonCaps > RHID_MAX_BUTTON_CAPS) {
						RHID_ERR("the number of button caps is larger than the "
								 "maximum supported");

						CloseHandle(devices[i].handle);
						devices[i].handle = NULL;
						HidD_FreePreparsedData(preparsed);
						continue;
					}

					int btn_desc_idx = 0;
					for(int k = 0; k < dev_caps.NumberInputButtonCaps; k++) {
						devices[i].button_descriptors[btn_desc_idx].report_id =
							button_caps[k].ReportID;

						if(button_caps[k].IsRange == TRUE) {
							for(uint16_t u = button_caps[k].Range.UsageMin;
								u <= button_caps[k].Range.UsageMax; u++) {
								devices[i]
									.button_descriptors[btn_desc_idx]
									.page = button_caps[k].UsagePage;
								devices[i]
									.button_descriptors[btn_desc_idx]
									.usage = u;
								// TODO confirm that this is the correct index.
								devices[i]
									.button_descriptors[btn_desc_idx]
									.index = btn_desc_idx;

								btn_desc_idx++;
							}
						}
						else {
							devices[i].button_descriptors[btn_desc_idx].page =
								button_caps[k].UsagePage;
							devices[i].button_descriptors[btn_desc_idx].usage =
								button_caps[k].NotRange.Usage;

							// TODO confirm that this is the correct index.
							devices[i].button_descriptors[btn_desc_idx].index =
								btn_desc_idx;
						}

						btn_desc_idx++;
					}
				}
			}
		}

		// get value caps.
		if(dev_caps.NumberInputValueCaps > 0) {
			if(_rhid_win_gcache.value_caps_count <
			   dev_caps.NumberInputValueCaps) {
				if(_rhid_win_gcache.value_caps == NULL) {
					_rhid_win_gcache.value_caps =
						malloc(sizeof(HIDP_VALUE_CAPS) *
							   dev_caps.NumberInputValueCaps);
				}
				else {
					_rhid_win_gcache.value_caps =
						realloc(_rhid_win_gcache.value_caps,
								sizeof(HIDP_VALUE_CAPS) *
									dev_caps.NumberInputValueCaps);
				}

				_rhid_win_gcache.value_caps_count =
					dev_caps.NumberInputValueCaps;
			}

			HIDP_VALUE_CAPS* value_caps = _rhid_win_gcache.value_caps;
			devices[i].cap_value_count	= dev_caps.NumberInputValueCaps;
			{
				ulong ret = HidP_GetValueCaps(
					HidP_Input, value_caps,
					(PUSHORT) &devices[i].cap_value_count, preparsed);
				if(ret != HIDP_STATUS_SUCCESS) {
					RHID_ERR(
						"failed to get device's value capabilities error: %s",
						_rhid_hidp_err_to_str(ret));
				}
				else {
					// assign value report ids, page, usage, min/max, and index.
					if(dev_caps.NumberInputValueCaps > RHID_MAX_VALUE_CAPS) {
						RHID_ERR("the number of value caps is larger "
								 "than the maximum supported");

						CloseHandle(devices[i].handle);
						devices[i].handle = NULL;
						HidD_FreePreparsedData(preparsed);
						continue;
					}

					for(int k = 0; k < dev_caps.NumberInputValueCaps; k++) {
						devices[i].value_descriptors[k].report_id =
							value_caps[k].ReportID;
						devices[i].value_descriptors[k].page =
							value_caps[k].UsagePage;

						if(value_caps[k].IsRange == TRUE) {
							RHID_ERR("ranged values not supported");
							devices[i].value_descriptors[k].usage =
								value_caps[k].Range.UsageMax;
						}
						else {
							devices[i].value_descriptors[k].usage =
								value_caps[k].NotRange.Usage;
						}

						devices[i].value_descriptors[k].logical_min =
							value_caps[k].LogicalMin;
						devices[i].value_descriptors[k].logical_max =
							value_caps[k].LogicalMax;

						devices[i].value_descriptors[k].index = k;
					}
				}
			}
		}

		devices[i].usage_page = dev_caps.UsagePage;
		devices[i].usage	  = dev_caps.Usage;

		// devices[i].cap_button_count = dev_caps.NumberInputButtonCaps;
		// devices[i].cap_value_count = dev_caps.NumberInputValueCaps;

		devices[i].button_count =
			HidP_MaxUsageListLength(HidP_Input, 0, preparsed);
		devices[i].value_count = dev_caps.NumberInputValueCaps;

		// note that we shouldn't allocate the report array here as that
		// wouldn't make all that much sense to the user. instead, allocate the
		// report in rhid_device_open.
		devices[i].report_size = dev_caps.InputReportByteLength;

		HidD_FreePreparsedData(preparsed);

		CloseHandle(devices[i].handle);
		devices[i].handle = NULL;
	}

	// free device list.
	SetupDiDestroyDeviceInfoList(dev_list);

	return 0;
}

int rhid_select_count(rhid_device_t* devices, int count,
					  rhid_select_func_t select_func) {
	int new_count = 0;

	// count the space required for this new list.
	for(int i = 0; i < count; i++) {
		if(select_func(devices[i].usage_page, devices[i].usage) == 1) {
			new_count++;
		}
	}

	return new_count;
}

int rhid_select_devices(rhid_device_t* devices, int count,
						rhid_device_t** selected, int selected_count,
						rhid_select_func_t select_func) {
	int select_index = 0;

	// populate the select list with pointers to devices who match the selection
	// requirments demended from the select function.
	for(int i = 0; i < count; i++) {
		if(select_func(devices[i].usage_page, devices[i].usage) == 1) {
			if(select_index > selected_count) {
				RHID_ERR("couldn't select all devices as the selection "
						 "count was not big enough");
				return -1;
			}
			selected[select_index] = &devices[i];
			select_index++;
		}
	}

	return 0;
}

static int rhid_read_report(rhid_device_t* device, uint8_t report_id) {
	unsigned long bytes_read = 0;

	if(device->native->is_reading) {
		// check to see if read is done.
		GetOverlappedResult(device->handle, &_rhid_win_gcache.report_overlapped,
							&bytes_read, FALSE);

		if(bytes_read >= device->report_size) {
			device->native->is_reading = 0;
		}
		else {
			return 0;
		}
	}

	// set the first byte to be the report id as specified in the docs.
	device->report[0] = report_id;

	if(ReadFile(device->handle, device->report, device->report_size,
				&bytes_read, &_rhid_win_gcache.report_overlapped) == FALSE) {
		if(GetLastError() != ERROR_IO_PENDING) {
			RHID_ERR_SYS("didn't read a device report", (int) GetLastError());
			// TODO think about doing return -1 instead. Note that this will
			// make any if statements return true which is wierd.
			return 0;
		}
	}

	if(device->native->is_reading == 0) {
		device->native->is_reading = 1;
		return 1;
	}

	return 0;
}

int rhid_open(rhid_device_t* device) {
	// open the file while trying different share options.
	device->handle = _rhid_open_device_handle(
		device->path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ);
	if(device->handle == NULL) {
		device->handle =
			_rhid_open_device_handle(device->path, GENERIC_READ | GENERIC_WRITE,
									 FILE_SHARE_READ | FILE_SHARE_WRITE);

		if(device->handle == NULL) {
			return -1;
		}
	}

	// get preparsed data from the device.
	PHIDP_PREPARSED_DATA preparsed = {0};
	if(HidD_GetPreparsedData(device->handle, &preparsed) == FALSE) {
		RHID_ERR("failed to get pre-parsed data from device");
		CloseHandle(device->handle);
		device->handle = NULL;
		return -1;
	}

	if(HidD_FlushQueue(device->handle) == FALSE) {
		RHID_ERR_SYS("failed to flush the device.", (int) GetLastError());
	}

	device->_preparsed = preparsed;
	device->report	   = malloc(device->report_size);

	device->buttons = calloc(device->button_count, sizeof(uint8_t));
	device->values	= calloc(device->value_count, sizeof(uint32_t));

	device->native			   = calloc(1, sizeof(rhid_native_t));
	device->native->is_reading = 0;

	// read initial report.
	if(HidD_GetInputReport(device->handle, device->report, device->report_size) == FALSE) {
		RHID_ERR_SYS("failed to get initial input report", GetLastError());
	}
	else {
		// TODO read initial report values.
	}

	device->is_open = 1;

	return 0;
}

int rhid_close(rhid_device_t* device) {
	if(device->is_open == 0) {
		return -1;
	}

	if(device->handle != NULL) {
		CloseHandle(device->handle);
		device->handle = NULL;
	}

	if(device->_preparsed != NULL) {
		HidD_FreePreparsedData(device->_preparsed);
		device->_preparsed = NULL;
	}

	if(device->report != NULL) {
		free(device->report);
		device->report = NULL;
	}

	if(device->buttons != NULL) {
		free(device->buttons);
		device->buttons = NULL;
	}

	if(device->values != NULL) {
		free(device->values);
		device->values = NULL;
	}

	if(device->native != NULL) {
		free(device->native);
		device->native = NULL;
	}

	device->is_open = 0;

	return 0;
}

int rhid_report(rhid_device_t* device, uint8_t report_id) {
	// If the device isn't open, error out.
	if(device->handle == NULL || device->is_open == 0) {
		RHID_ERR("can't get a report because the device isn't open");
		return -1;
	}

	// read report.
	int report_avaliable = rhid_read_report(device, report_id);

	// parse button data from report.
	ulong		   active_count					  = MAX_BUTTON_COUNT;
	USAGE_AND_PAGE usages_pages[MAX_BUTTON_COUNT] = {0};
	if(report_avaliable) {
		ulong ret = HidP_GetUsagesEx(HidP_Input, 0, usages_pages, &active_count,
									 (PHIDP_PREPARSED_DATA) device->_preparsed,
									 device->report, device->report_size);
		if(ret != HIDP_STATUS_SUCCESS) {
			RHID_ERR("failed to parse button data from report error: %s",
					 _rhid_hidp_err_to_str(ret));
			return -1;
		}

		memset(device->buttons, 0, device->button_count);

		for(int j = 0; j < active_count; j++) {
			for(int i = 0; i < device->button_count; i++) {
				if(device->button_descriptors[i].page ==
					   usages_pages[j].UsagePage &&
				   device->button_descriptors[i].usage ==
					   usages_pages[j].Usage) {
					device->buttons[device->button_descriptors[i].index] = 1;
					break;
				}
			}
		}

		/*uint8_t button_temp[MAX_BUTTON_COUNT] = {0};

		// update temp buffer.
		for(int j = 0; j < active_count; j++) {
			for(int i = 0; i < device->button_count; i++) {
				if(device->button_descriptors[i].page ==
					   usages_pages[j].UsagePage &&
				   device->button_descriptors[i].usage ==
					   usages_pages[j].Usage) {
					button_temp[device->button_descriptors[i].index] = 1;
				}
			}
		}

		for(int i = 0; i < device->button_count; i++) {
			switch(device->buttons[i]) {
				case RHID_BUTTON_OFF:
					device->buttons[i] =
						button_temp[i] ? RHID_BUTTON_PRESSED : RHID_BUTTON_OFF;
					break;

				case RHID_BUTTON_HELD:
					device->buttons[i] = button_temp[i] ? RHID_BUTTON_HELD
														: RHID_BUTTON_RELEASED;
					break;

				case RHID_BUTTON_PRESSED:
					device->buttons[i] = button_temp[i] ? RHID_BUTTON_HELD
														: RHID_BUTTON_RELEASED;
					break;

				case RHID_BUTTON_RELEASED:
					device->buttons[i] =
						button_temp[i] ? RHID_BUTTON_PRESSED : RHID_BUTTON_OFF;
					break;
			}
		}*/
	}

	if(report_avaliable == 0) {
		return 0;
	}

	// parse value data from report.
	for(int i = 0; i < device->value_count; i++) {
		ulong ret = HidP_GetUsageValue(
			HidP_Input, device->value_descriptors[i].page, 0,
			device->value_descriptors[i].usage, (PULONG) &device->values[i],
			device->_preparsed, device->report, device->report_size);

		if(ret != HIDP_STATUS_SUCCESS) {
			RHID_ERR("failed to parse value data from report error %s",
					 _rhid_hidp_err_to_str(ret));
		}
	}

	return 0;
}

int rhid_get_buttons_state(rhid_device_t* device, uint8_t* buttons, int size) {
	if(size < device->button_count) {
		return -1;
	}

	memcpy(buttons, device->buttons, device->button_count);

	return 0;
}
int rhid_get_values_state(rhid_device_t* device, uint32_t* values, int size) {
	if(size < device->value_count) {
		return -1;
	}

	memcpy(values, device->values, device->value_count * sizeof(uint32_t));

	return 0;
}

int rhid_get_buttons_usage(rhid_device_t* device, uint16_t* usages, int size) {
	// device->
}

int rhid_get_values_usage(rhid_device_t* device, uint16_t* usages, int size) {
}

int rhid_get_button(rhid_device_t* device, uint16_t usage) {
	return 0;
}
int rhid_get_value(rhid_device_t* device, uint16_t usage) {
	return 0;
}

int rhid_get_button_count(rhid_device_t* device) {
	return device->button_count;
}
int rhid_get_value_count(rhid_device_t* device) {
	return device->value_count;
}

int rhid_is_open(rhid_device_t* device) {
	return device->is_open;
}

uint16_t rhid_get_vendor_id(rhid_device_t* device) {
	return device->vendor_id;
}
uint16_t rhid_get_product_id(rhid_device_t* device) {
	return device->product_id;
}

uint16_t rhid_get_usage_page(rhid_device_t* device) {
	return device->usage_page;
}
uint16_t rhid_get_usage(rhid_device_t* device) {
	return device->usage;
}

const char* rhid_get_manufacturer_name(rhid_device_t* device) {
	return device->manufacturer_name;
}
const char* rhid_get_product_name(rhid_device_t* device) {
	return device->product_name;
}
