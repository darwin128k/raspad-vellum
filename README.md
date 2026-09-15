# revloader

Replacement for `cstrike.exe` for No-Steam GoldSrc (RevEmu). Does not know about external mods: either brings up Steam and launches a foreign `hl.exe`, or, when built as `standalone`, pulls in the sources from the `hl` repository and becomes the game process itself.

## Modes

**Loader (default).** Like RevLoader 2014: Steam IPC, `steam.dll` in this process, `CreateProcess` on `ProcName` from `rev.ini` (usually `hl.exe -game cstrike`), waits for `hl.exe` to exit. The loader pid stays the "live Steam".

**Standalone.** Same Steam, then `HlLauncher_Run` from the `hl` repository in the same process. A separate `hl.exe` is not needed.

## Build

32-bit MSVC.

Loader:

```bat
build.bat
```

Standalone (`hl` path defaults to `..\hl`):

```bat
build.bat standalone
build.bat standalone D:\src\hl
```

CMake:

```bat
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DREVLOADER_STANDALONE=ON -DHL_DIR=C:/src/hl -B build
cmake --build build
```

The script places `cstrike.exe` into the game root (looks for `hw.dll` one or two levels up).

## Run

```bat
cstrike.exe
```

## License

MIT. See [LICENSE](LICENSE).