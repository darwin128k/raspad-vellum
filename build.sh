#!/bin/sh
set -e
cd "$(dirname "$0")"

if ! echo 'int main(){return 0;}' | g++ -m32 -x c++ - -o /tmp/vellum-m32-test >/dev/null 2>&1; then
    echo "Need a 32-bit g++ toolchain (Debian/Ubuntu: sudo apt install g++-multilib gcc-multilib)"
    exit 1
fi
rm -f /tmp/vellum-m32-test

STANDALONE=OFF
HL_DIR=""
REVEMU2013=OFF
for arg in "$@"; do
    case "$arg" in
        standalone) STANDALONE=ON ;;
        revemu2013) REVEMU2013=ON ;;
        metahook|nometahook)
            echo "metahook is Windows-only"
            exit 1
            ;;
        *)
            if [ "$STANDALONE" = ON ] && [ -z "$HL_DIR" ]; then
                HL_DIR=$arg
            else
                echo "Unknown argument: $arg"
                exit 1
            fi
            ;;
    esac
done

if [ "$STANDALONE" = ON ] && [ -z "$HL_DIR" ]; then
    if [ -f ../raspad-hl/src/launcher.cpp ]; then
        HL_DIR=../raspad-hl
    else
        HL_DIR=../hl
    fi
fi
if [ "$STANDALONE" = ON ]; then
    HL_DIR=$(cd "$HL_DIR" && pwd)
fi

mkdir -p build-linux
cd build-linux
if [ "$STANDALONE" = ON ]; then
    cmake -DCMAKE_BUILD_TYPE=Release -DREVLOADER_STANDALONE=ON -DHL_DIR="$HL_DIR" -DVELLUM_AUTH_REVEMU2013=$REVEMU2013 ..
else
    cmake -DCMAKE_BUILD_TYPE=Release -DREVLOADER_STANDALONE=OFF -DVELLUM_AUTH_REVEMU2013=$REVEMU2013 ..
fi
cmake --build .
if [ "$STANDALONE" = ON ]; then
    echo "Build OK: build-linux/cstrike_linux build-linux/steamclient.so (standalone, hl from $HL_DIR, REVEMU2013=$REVEMU2013)"
else
    echo "Build OK: build-linux/cstrike_linux build-linux/steamclient.so (REVEMU2013=$REVEMU2013)"
fi
