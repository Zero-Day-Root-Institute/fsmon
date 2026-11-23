/* Exercise the iOS metadata path on macOS with a fake system library. */
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>

static CFTypeRef answer;
static bool library_available = true, symbol_available = true;
static unsigned int opens, copies, closes;

static CFTypeRef test_copy_answer(CFStringRef key, CFDictionaryRef options) {
	assert(CFEqual(key, CFSTR("UniqueDeviceID")) && !options);
	copies++;
	return answer ? CFRetain(answer) : NULL;
}

static void *test_dlopen(const char *path, int flags) {
	assert(!strcmp(path, "/usr/lib/libMobileGestalt.dylib"));
	assert(flags == (RTLD_LAZY | RTLD_LOCAL));
	opens++;
	return library_available ? &opens : NULL;
}

static void *test_dlsym(void *handle, const char *symbol) {
	assert(handle == &opens && !strcmp(symbol, "MGCopyAnswer"));
	return symbol_available ? (void *)test_copy_answer : NULL;
}

static int test_dlclose(void *handle) {
	assert(handle == &opens);
	closes++;
	return 0;
}

#define FSMOD_IOS_METADATA_TEST 1
#define FSMOD_CFNETWORK_TEST 1
#define dlopen test_dlopen
#define dlsym test_dlsym
#define dlclose test_dlclose
#include "../report.c"
#undef dlopen
#undef dlsym
#undef dlclose

const char *fm_typestr(int type) {
	(void)type;
	return "FSE_CREATE_FILE";
}

static void test_identifier_values(void) {
	assert(copy_ios_identifier(CFSTR("0123456789abcdef0123456789abcdef01234567")));
	assert(!strcmp(host_info.uuid, "0123456789abcdef0123456789abcdef01234567"));
	assert(copy_ios_identifier(CFSTR("00008110-00123456789ABCDE")));
	assert(!strcmp(host_info.uuid, "00008110-00123456789ABCDE"));
	assert(!copy_ios_identifier(NULL));
	assert(!copy_ios_identifier(kCFBooleanTrue));
	assert(!copy_ios_identifier(CFSTR("")));
	assert(!copy_ios_identifier(CFSTR("bad id")));
	assert(!copy_ios_identifier(CFSTR("bad\nid")));
	assert(!copy_ios_identifier(CFSTR("\u4e2d")));
	const UniChar embedded_null[] = {'a', 0, 'b'};
	CFStringRef invalid = CFStringCreateWithCharacters(NULL, embedded_null, 3);
	assert(invalid && !copy_ios_identifier(invalid));
	CFRelease(invalid);
	char oversized[129];
	memset(oversized, 'a', sizeof(oversized) - 1);
	oversized[sizeof(oversized) - 1] = 0;
	invalid = CFStringCreateWithCString(NULL, oversized, kCFStringEncodingASCII);
	assert(invalid && !copy_ios_identifier(invalid));
	CFRelease(invalid);
}

static void test_loader_failures(void) {
	host_info.uuid[0] = 0;
	library_available = false;
	ios_device_identifier();
	assert(!host_info.uuid[0] && opens == 1 && !closes && !copies);
	library_available = true;
	symbol_available = false;
	ios_device_identifier();
	assert(!host_info.uuid[0] && opens == 2 && closes == 1 && !copies);
	symbol_available = true;
	answer = NULL;
	ios_device_identifier();
	assert(!host_info.uuid[0] && opens == 3 && closes == 2 && copies == 1);
	answer = kCFBooleanTrue;
	ios_device_identifier();
	assert(!host_info.uuid[0] && opens == 4 && closes == 3 && copies == 2);
}

static void test_cached_event_identity(void) {
	answer = CFSTR("0123456789abcdef0123456789abcdef01234567");
	unsigned int before = copies;
	FileMonitor monitor = {0};
	monitor.backend.name = "devfsev";
	FileMonitorEvent event = {0};
	event.file = "/private/var/mobile/probe";
	event.uid = 424242;
	event.gid = 501;
	for (int i = 0; i < 3; i++) {
		char *json = fm_event_json(&monitor, &event);
		assert(json && strstr(json, "\"uuid\":\"0123456789abcdef0123456789abcdef01234567\""));
		assert(strstr(json, "\"os_name\":\"iOS\""));
		assert(strstr(json, "\"uid\":424242,\"gid\":501"));
		assert(copies == before + 1);
		free(json);
	}
}

int main(void) {
	test_identifier_values();
	test_loader_failures();
	test_cached_event_identity();
	puts("iOS metadata tests passed (mock MobileGestalt)");
	return 0;
}
