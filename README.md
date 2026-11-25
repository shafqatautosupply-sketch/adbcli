# adbi – ADB over Wi-Fi for iPhone 4S / iOS 6 (armv7)

Implements the modern ADB wireless-debugging protocol (Android 11+, dynamic port, **not** 5555):
TLS 1.3 pairing (SPAKE2 + AES-128-GCM) and the `STLS` TLS-upgraded ADB connection, plus
`adb shell`, push/pull, install, screencap and Android-TV-friendly key/text/tap helpers.
Old `adb tcpip` (port 5555, RSA AUTH) devices work too.

## Why OpenSSL 1.1.1
iOS 6's Secure Transport has no TLS 1.3, and pairing needs TLS 1.3 *keying-material export*.
So adbi statically links OpenSSL 1.1.1 (only dependency; SPAKE2 is done with OpenSSL BIGNUM).

## Build (on the phone, Theos + iOS 6.1 SDK)
    SDK=$THEOS/sdks/iPhoneOS6.1.sdk sh build-deps.sh   # once; downloads OpenSSL itself, ~10-20 min
    make package install        # Theos       (or: sh build-simple.sh)

## Use
On the Android device: Developer options > **Wireless debugging** > on.
    adbi discover                         # lists devices + pairing services (mDNS)
    adbi pair                             # auto-finds device showing the pairing dialog, asks the code
    adbi pair 192.168.1.50:37123 123456   # or give pairing IP:PORT (from dialog) and 6-digit code
    adbi connect                          # auto-discovers connect port (it changes on every toggle)
    adbi connect 192.168.1.50:41234
    adbi shell                            # interactive (pty, window resize)
    adbi shell pm list packages -3
    adbi key home | adbi key up up ok | adbi text "hello world" | adbi tap 500 300
    adbi push a.apk /sdcard/ ; adbi pull /sdcard/x.png ; adbi install -r app.apk
    adbi screencap shot.png
    adbi -s 192.168.1.50:41234 shell     # explicit target (else last 'connect' / $ADBI_SERIAL)
Key lives in ~/.adbi/adbkey (override: ADBI_KEY=/path; you can reuse a PC's ~/.android/adbkey to skip
the slow on-device RSA keygen). Pairing is stored on the device, per key.

## Tested
Desktop Linux/OpenSSL 3 against BoringSSL: SPAKE2 interop (60 runs, both roles), a mock adbd that does
pairing + STLS + shell v2 + sync (see test/). Not yet run on a real Android device or on iOS 6.
