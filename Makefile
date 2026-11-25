# Theos build for iPhone 4S / iOS 6 (armv7).  Needs OpenSSL 1.1.1 built by build-deps.sh
ARCHS = armv7
TARGET = iphone:clang:latest:6.0
OPENSSL ?= $(HOME)/ios6-openssl

include $(THEOS)/makefiles/common.mk

TOOL_NAME = adbi
adbi_FILES = src/main.c src/conn.c src/keys.c src/pairing.c src/mdns.c src/spake2.c
adbi_CFLAGS = -I$(OPENSSL)/include -DHAVE_DNSSD -Wno-deprecated-declarations -O2
adbi_LDFLAGS = $(OPENSSL)/lib/libssl.a $(OPENSSL)/lib/libcrypto.a

include $(THEOS_MAKE_PATH)/tool.mk
