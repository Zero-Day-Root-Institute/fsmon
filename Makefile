ARCHS?=arm64

CFLAGS+=-I.
CFLAGS+=-Wall
CFLAGS+=-pthread
LDFLAGS+=-pthread

include config.mk
CFLAGS+=-DFSMOD_VERSION=\"$(VERSION)\"

SOURCES=main.c util.c config.c service.c report.c control.c
SOURCES+=backend/*.c

TARGET_TRIPLE := $(shell $(CC) -dumpmachine 2>/dev/null)

# Native desktop reporting runs in-process with libcurl. Override these for a
# target sysroot; never use host pkg-config output when cross-compiling.
CURL_CFLAGS?=$(shell pkg-config --cflags libcurl 2>/dev/null || curl-config --cflags 2>/dev/null)
CURL_LIBS?=$(shell pkg-config --libs libcurl 2>/dev/null || curl-config --libs 2>/dev/null || echo -lcurl)
REPORT_CFLAGS=$(CURL_CFLAGS)
REPORT_LIBS=$(CURL_LIBS)

# The NDK does not include libcurl. Keep default Android monitoring builds
# independent of desktop libraries; opt in only with target libcurl flags.
ANDROID_REPORT?=0
ANDROID_CURL_CFLAGS?=
ANDROID_CURL_LIBS?=
ANDROID_REPORT_CFLAGS=-DFSMOD_NO_REPORT=1
ANDROID_REPORT_LIBS=
ifeq ($(ANDROID_REPORT),1)
ifeq ($(strip $(ANDROID_CURL_LIBS)),)
$(error ANDROID_REPORT=1 requires ANDROID_CURL_LIBS for a target-built libcurl)
endif
ANDROID_REPORT_CFLAGS=$(ANDROID_CURL_CFLAGS)
ANDROID_REPORT_LIBS=$(ANDROID_CURL_LIBS)
endif
ifneq ($(findstring android,$(TARGET_TRIPLE)),)
REPORT_CFLAGS=$(ANDROID_REPORT_CFLAGS)
REPORT_LIBS=$(ANDROID_REPORT_LIBS)
endif

ifneq ($(findstring -darwin,$(TARGET_TRIPLE)),)
	TARGET_OS_TYPE=Darwin
else ifneq ($(findstring -linux,$(TARGET_TRIPLE)),)
	TARGET_OS_TYPE=Linux
else ifneq ($(findstring -android,$(TARGET_TRIPLE)),)
	TARGET_OS_TYPE=Linux
else
	TARGET_OS_TYPE=$(shell uname)
endif

ifeq ($(TARGET_OS_TYPE),Linux)

# LINUX: GNU / ANDROID
#     __
#  -=(o '.
#     \.-.\
#     /|  \\
#     '|  ||
#      _\_):,_

FANOTIFY_CFLAGS+=-DHAVE_FANOTIFY=1
FANOTIFY_CFLAGS+=-DHAVE_SYS_FANOTIFY=1

all: fsmod

fsmod:
	$(CC) -o fsmod $(CFLAGS) $(FANOTIFY_CFLAGS) $(REPORT_CFLAGS) $(SOURCES) $(LDFLAGS) $(REPORT_LIBS)

DESTDIR?=
PREFIX?=/usr

clean:
	rm -f fsmod
	rm -rf fsmod-macos* fsmod-ios* fsmod-wch*
	rm -rf fsmod-and*
else
# APPLE: OSX / IOS / IWATCH
#     _
#    _\)/_
#   /     \
#   \     /
#    \_._/

DESTDIR?=
PREFIX?=/usr/local

# iOS
IOS_ARCHS=$(addprefix -arch ,$(ARCHS))
IOS_MIN_VERSION?=12.0
IOS_CFLAGS+=$(IOS_ARCHS)
IOS_CFLAGS+=-isysroot ${IOS_SYSROOT}
IOS_CFLAGS+=-flto
IOS_CFLAGS+=-target arm64-apple-ios$(IOS_MIN_VERSION)
IOS_CFLAGS+=-miphoneos-version-min=$(IOS_MIN_VERSION)
IOS_CFLAGS+=-O3 -Wall -Wno-deprecated-declarations
ifeq ($(shell uname -m | grep -E "iPhone|iPad|iPod" > /dev/null ; echo $${?}),0)
IOS_ON_DEVICE_COMPILE=1
IOS_SYSROOT=/
IOS_CC=/usr/bin/clang
IOS_STRIP=/usr/bin/strip
LDID=/usr/bin/ldid
else
IOS_ON_DEVICE_COMPILE=0
IOS_SYSROOT=$(shell xcrun --sdk iphoneos --show-sdk-path)
IOS_CC=$(shell xcrun --sdk iphoneos --find clang) $(IOS_CFLAGS)
IOS_STRIP=xcrun --sdk iphoneos strip
LDID?=$(shell command -v ldid 2>/dev/null || echo bin/ldid_macosx_$(shell uname -m))
endif

# iWatch
WCH_CFLAGS=-arch armv7k
WCH_SYSROOT=$(shell xcrun --sdk watchos --show-sdk-path)
WCH_CFLAGS+=-isysroot ${WCH_SYSROOT}
WCH_CC=$(shell xcrun --sdk iphoneos --find clang) $(WCH_CFLAGS)

CC?=gcc
CFLAGS+=-g -ggdb

OBJS=fsmod.o main.o

all: macos

oldios:
	$(IOS_CC) $(CFLAGS) -DTARGET_IOS=1 -o fsmod-ios $(SOURCES) -framework CoreFoundation -framework MobileCoreServices
	$(IOS_STRIP) fsmod-ios
	if [ $(IOS_ON_DEVICE_COMPILE) != 1 ]; then \
		xcrun --sdk iphoneos codesign -f --entitlements ./entitlements.plist -s- fsmod-ios; fi

IOS_FRAMEWORKS=-framework CoreFoundation -framework CFNetwork
ios:
	$(IOS_CC) $(CFLAGS) -DTARGET_IOS=1 -o fsmod-ios $(SOURCES) $(IOS_FRAMEWORKS)
	ls -l fsmod-ios
	-$(IOS_STRIP) fsmod-ios
	$(LDID) -Sentitlements.plist fsmod-ios

ios-packages: ios
	python3 dist/ios/package.py --binary fsmod-ios --variant both

# Standalone on-device diagnostic. It creates only a unique test directory.
ios-copy-probe:
	mkdir -p out/ios
	$(IOS_CC) $(CFLAGS) -Wextra -Werror -o out/ios/fsmod-copy-probe-arm64 dist/ios/copy_probe.c
	$(IOS_STRIP) out/ios/fsmod-copy-probe-arm64
	$(LDID) -Sentitlements.plist out/ios/fsmod-copy-probe-arm64
.PHONY: ios-copy-probe

ios2:
	$(MAKE) ios
	$(MAKE) ios-patch

ios-patch:
	rabin2 -x fsmod-ios
	export a=fsmod-ios.fat/fsmod-ios.arm_64* ; \
		export OFF=`rabin2 -H $$a | grep -C 2 /CoreSer | head -n1 | cut -d ' ' -f 1`; \
		echo OFF=$$OFF ; \
		r2 -qnwc "wx 18000080 @ $$OFF-4" $$a
	rm -f fsmod-ios
	lipo -create -arch arm64 fsmod-ios.fat/fsmod-ios.arm_64* -arch armv7 fsmod-ios.fat/fsmod-ios.arm_32* -output fsmod-ios
	-xcrun --sdk iphoneos codesign -f --entitlements ./entitlements.plist -s- fsmod-ios
	rm -rf fsmod-ios.fat

cydia: ios
	rm -rf dist/cydia/out
	$(MAKE) -C dist/cydia

macos:
	$(CC) $(CFLAGS) $(REPORT_CFLAGS) -mmacosx-version-min=10.12 -DTARGET_OSX=1 -o fsmod-macos $(SOURCES) $(LDFLAGS) -framework CoreServices $(REPORT_LIBS)
	strip fsmod-macos
	cp -f fsmod-macos fsmod

macos-pkg:
	./pkg.sh

wch:
	$(WCH_CC) $(CFLAGS) -DTARGET_WATCHOS=1 -o fsmod-wch $(SOURCES)

fat: ios macos wch
	lipo fsmod-ios -thin armv7 -output fsmod-ios-armv7
	lipo fsmod-ios -thin arm64 -output fsmod-ios-arm64
	lipo -create -output fsmod \
		-arch arm64 fsmod-ios-arm64 \
		-arch armv7 fsmod-ios-armv7 \
		-arch armv7k fsmod-wch \
		-arch x86_64 fsmod-macos
	strip fsmod
	codesign -s- fsmod

clean:
	rm -f fsmod-macos fsmod-ios
	rm -rf fsmod*.dSYM
	rm -f fsmod-and*

.PHONY: cydia ios ios-packages macos macos-pkg fat wch

endif

SBINDIR=$(DESTDIR)/$(PREFIX)/sbin
MANDIR=$(DESTDIR)/$(PREFIX)/share/man/man1

install:
	mkdir -p $(SBINDIR)
	install -m 0755 fsmod $(SBINDIR)/fsmod
	mkdir -p $(MANDIR)
	install -m 0644 fsmod.1 $(MANDIR)/fsmod.1

uninstall:
	rm -f $(SBINDIR)/fsmod
	rm -f $(MANDIR)/fsmod.1

# ANDROID
#
# \.-----./
# / o   o \
# `-------'

KITKAT_CFLAGS=-DHAVE_FANOTIFY=0 -DHAVE_SYS_FANOTIFY=0
LOLLIPOP_CFLAGS=-DHAVE_FANOTIFY=1 -DHAVE_SYS_FANOTIFY=0

NDK_ARCH?=
ANDROID_ARCHS=arm arm64 x86 x86_64
ANDROID_API?=

ifneq ($(NDK_ARCH),)
ANDROID_ARCHS=$(NDK_ARCH)
endif
ifeq ($(ANDROID_API),)
AAPIMODE=aagt21compile
else
AAPIMODE=$(shell test ${ANDROID_API} -gt 21 && echo aagt21compile || echo aalt21compile)
endif

andarm64:
	sh android-shell.sh arm64 make android

and android:
	for a in $(ANDROID_ARCHS) ; do \
		if [ -z "${NDK}" ] ; then \
			./android-shell.sh $$a \
			$(MAKE) $(AAPIMODE) ANDROID_API=$(ANDROID_API) NDK_ARCH=$$a ; \
		else \
			$(MAKE) $(AAPIMODE) ANDROID_API=$(ANDROID_API) NDK_ARCH=$$a ; \
		fi; \
	done

aagt21compile:
	ndk-gcc $(ANDROID_API) $(LOLLIPOP_CFLAGS) $(CFLAGS) $(ANDROID_REPORT_CFLAGS) -o fsmod-and$(ANDROID_API)-$(NDK_ARCH) $(SOURCES) $(LDFLAGS) $(ANDROID_REPORT_LIBS)

aalt21compile:
	ndk-gcc $(ANDROID_API) $(KITKAT_CFLAGS) $(CFLAGS) $(ANDROID_REPORT_CFLAGS) -o fsmod-and$(ANDROID_API)-$(NDK_ARCH) $(SOURCES) $(LDFLAGS) $(ANDROID_REPORT_LIBS)

brewinstall:
	brew reinstall --HEAD ./fsmod.rb

dos:
	brew install --build-from-source fsmod.rb

brewcheck:
	brew audit --strict fsmod.rb
	brew test fsmod

.PHONY: all fsmod clean
.PHONY: install uninstall
.PHONY: and android brewinstall

# Portable configuration/reporting/service unit tests (Linux and macOS).
TEST_BINS=tests/test_config tests/test_report tests/test_service tests/test_control tests/test_process_filter tests/test_default_path
DEFAULT_STARTUP_CONFIG=$(CURDIR)/tests/.default-startup.conf
DEFAULT_STARTUP_FIXTURE=tests/fsmod-default-startup
PROCESS_TEST_LIBS=
ifeq ($(TARGET_OS_TYPE),Darwin)
PROCESS_TEST_LIBS=-framework CoreServices
endif
ifeq ($(TARGET_OS_TYPE),Darwin)
REPORT_TEST_LIBS=-framework CoreFoundation
tests/test_report_cfnetwork: report.c report.h config.h fsmod.h tests/test_report.c
	$(CC) $(CFLAGS) -DFSMOD_CFNETWORK_TEST=1 -Wno-deprecated-declarations -Wextra -Werror -o $@ report.c tests/test_report.c $(LDFLAGS) -framework CoreFoundation -framework CFNetwork
test-cfnetwork: tests/test_report_cfnetwork
	./tests/test_report_cfnetwork
tests/test_ios_metadata: report.c report.h config.h fsmod.h tests/test_ios_metadata.c
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -Wextra -Werror -o $@ tests/test_ios_metadata.c $(LDFLAGS) -framework CoreFoundation -framework CFNetwork
test-ios-metadata: tests/test_ios_metadata
	./tests/test_ios_metadata
tests/test_devfsev: backend/devfsev.c backend/devfsev.h fsmod.h util.h tests/test_devfsev.c
	$(CC) $(CFLAGS) -Wextra -Werror -o $@ tests/test_devfsev.c $(LDFLAGS)
test-devfsev: tests/test_devfsev
	./tests/test_devfsev
endif
tests/test_config: config.c config.h backend/devfsev.h tests/test_config.c
	$(CC) $(CFLAGS) -Wextra -Werror -o $@ config.c tests/test_config.c $(LDFLAGS)
tests/test_report: report.c report.h config.h fsmod.h util.h backend/devfsev.h tests/test_report.c
	$(CC) $(CFLAGS) $(REPORT_CFLAGS) -Wextra -Werror -o $@ report.c tests/test_report.c $(LDFLAGS) $(REPORT_TEST_LIBS) $(REPORT_LIBS)
tests/test_service: service.c service.h tests/test_service.c
	$(CC) $(CFLAGS) -Wextra -Werror -o $@ service.c tests/test_service.c $(LDFLAGS)
tests/test_control: control.c control.h report.h config.h fsmod.h tests/test_control.c
	$(CC) $(CFLAGS) -Wextra -Werror -o $@ tests/test_control.c $(LDFLAGS)
tests/test_process_filter: main.c util.c config.c service.c report.c control.c config.h report.h service.h control.h fsmod.h tests/test_process_filter.c $(wildcard backend/*.c)
	$(CC) $(CFLAGS) $(FANOTIFY_CFLAGS) $(REPORT_CFLAGS) -o $@ tests/test_process_filter.c util.c config.c service.c report.c control.c backend/*.c $(LDFLAGS) $(PROCESS_TEST_LIBS) $(REPORT_LIBS)
tests/test_default_path: main.c util.c config.c service.c report.c control.c config.h report.h service.h control.h fsmod.h tests/test_default_path.c $(wildcard backend/*.c)
	$(CC) $(CFLAGS) $(FANOTIFY_CFLAGS) $(REPORT_CFLAGS) -o $@ tests/test_default_path.c util.c config.c service.c report.c control.c backend/*.c $(LDFLAGS) $(PROCESS_TEST_LIBS) $(REPORT_LIBS)
$(DEFAULT_STARTUP_FIXTURE): main.c util.c config.c service.c report.c control.c config.h report.h service.h control.h fsmod.h $(wildcard backend/*.c)
	$(CC) $(CFLAGS) $(FANOTIFY_CFLAGS) $(REPORT_CFLAGS) -DFSMOD_DEFAULT_CONFIG='"$(DEFAULT_STARTUP_CONFIG)"' -o $@ $(SOURCES) $(LDFLAGS) $(PROCESS_TEST_LIBS) $(REPORT_LIBS)
test-default-startup: $(DEFAULT_STARTUP_FIXTURE) tests/test_default_startup.py tests/test_daemon.py
	python3 tests/test_default_startup.py ./$(DEFAULT_STARTUP_FIXTURE) "$(DEFAULT_STARTUP_CONFIG)"
test: all $(TEST_BINS) test-default-startup
	./tests/test_config
	./tests/test_report
	./tests/test_service
	./tests/test_control
	./tests/test_process_filter
	./tests/test_default_path
	python3 tests/test_daemon.py ./fsmod
	python3 tests/test_control.py ./fsmod
	python3 tests/test_control_run_errors.py ./fsmod

test-control: all tests/test_control tests/test_control.py tests/test_control_run_errors.py
	./tests/test_control
	python3 tests/test_control.py ./fsmod
	python3 tests/test_control_run_errors.py ./fsmod
.PHONY: test test-default-startup test-control
test-ios-package:
	python3 tests/test_ios_package.py -v
.PHONY: test-ios-package
clean-tests:
	rm -f $(TEST_BINS) $(DEFAULT_STARTUP_FIXTURE) tests/test_report_cfnetwork tests/test_ios_metadata tests/test_devfsev
clean: clean-tests
.PHONY: clean-tests test-cfnetwork test-ios-metadata test-devfsev
