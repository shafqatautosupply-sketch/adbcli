#!/bin/sh
# Desktop (Linux) tests: needs BoringSSL built in $BS (cmake -GNinja .. && ninja crypto ssl)
BS=${BS:-$HOME/boringssl}
g++ -O2 -I$BS/include spake_ref.cc $BS/build/libcrypto.a -lpthread -o spake_ref
gcc -O2 -Wno-deprecated-declarations spake_mine.c ../src/spake2.c -lcrypto -o spake_mine
python3 spake_driver.py ./spake_ref ./spake_mine        # SPAKE2 interop vs BoringSSL
g++ -O1 -std=c++17 -I$BS/include mock_adbd.cc $BS/build/libssl.a $BS/build/libcrypto.a -lpthread -o mock_adbd
gcc -O2 -Wno-deprecated-declarations -DHAVE_DNSSD -o adbi_host ../src/*.c -lssl -lcrypto -ldns_sd
echo "mock device: ./mock_adbd pair 45001 123456 & ./adbi_host pair 127.0.0.1:45001 123456"
echo "             ./mock_adbd adb 45011 &  ./adbi_host -s 127.0.0.1:45011 shell echo hi"
