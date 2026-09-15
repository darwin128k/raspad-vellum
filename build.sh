#!/bin/sh
set -e
cd "$(dirname "$0")"

if ! echo 'int main(){return 0;}' | g++ -m32 -x c++ - -o /tmp/vellum-m32-test >/dev/null 2>&1; then
    echo "Need a 32-bit g++ toolchain (Debian/Ubuntu: sudo apt install g++-multilib gcc-multilib)"
    exit 1
fi
rm -f /tmp/vellum-m32-test

STANDALONE=OFF
LAUNCHER_DLLS=ON
HL_DIR=""
for arg in "$@"; do
    case "$arg" in
        nodll) LAUNCHER_DLLS=OFF ;;
        standalone) STANDALONE=ON ;;
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
    cmake -DCMAKE_BUILD_TYPE=Release -DREVLOADER_STANDALONE=ON -DREVLOADER_LAUNCHER_DLLS=$LAUNCHER_DLLS -DHL_DIR="$HL_DIR" ..
else
    cmake -DCMAKE_BUILD_TYPE=Release -DREVLOADER_STANDALONE=OFF -DREVLOADER_LAUNCHER_DLLS=$LAUNCHER_DLLS ..
fi
cmake --build .
if [ "$STANDALONE" = ON ]; then
    echo "Build OK: build-linux/cstrike_linux build-linux/steamclient.so (standalone, hl from $HL_DIR, REVLOADER_LAUNCHER_DLLS=$LAUNCHER_DLLS)"
else
    echo "Build OK: build-linux/cstrike_linux build-linux/steamclient.so (REVLOADER_LAUNCHER_DLLS=$LAUNCHER_DLLS)"
fi
