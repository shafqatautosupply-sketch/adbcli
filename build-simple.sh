#!/bin/sh
# Alternative to Theos: plain clang build.  SDK= iPhoneOS6.1.sdk, OPENSSL= prefix from build-deps.sh
set -e
SDK=${SDK:-$THEOS/sdks/iPhoneOS6.1.sdk}
OPENSSL=${OPENSSL:-$HOME/ios6-openssl}
clang -arch armv7 -isysroot "$SDK" -miphoneos-version-min=6.0 -O2 -DHAVE_DNSSD \
  -Wno-deprecated-declarations -I"$OPENSSL/include" \
  src/main.c src/conn.c src/keys.c src/pairing.c src/mdns.c src/spake2.c \
  "$OPENSSL/lib/libssl.a" "$OPENSSL/lib/libcrypto.a" -o adbi
ldid -S adbi 2>/dev/null || true
echo "built ./adbi  (copy to /usr/bin)"
