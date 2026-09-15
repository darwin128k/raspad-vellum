# revloader

Replacement for `cstrike.exe` for No-Steam GoldSrc, plus a `steamclient.dll` that
speaks a private **Vellum** ticket. Two CMake targets:

- **loader** (`cstrike.exe`) — Steam IPC, `steam.dll` in this process, then either
  `CreateProcess` on `ProcName` from `rev.ini` or in-process GoldSrc (standalone).
- **steamclient** (`steamclient.dll`) — `CreateInterface("SteamClient012")` and a
  64-byte Vellum auth blob. qproto classifies that blob as `CA_VELLUM`, distinct
  from RevEmu / SC2009 / OldRevEmu.

Does not know about external mods: either brings up Steam and launches a foreign
`hl.exe`, or, when built as `standalone`, pulls in the sources from the `hl`
repository and becomes the game process itself.

## Modes

**Loader (default).** Like RevLoader 2014: Steam IPC, `steam.dll` in this process,
`CreateProcess` on `ProcName` from `rev.ini` (usually `hl.exe -game cstrike`), waits
for `hl.exe` to exit. The loader pid stays the "live Steam".

**Standalone.** Same Steam, then `HlLauncher_Run` from the `hl` repository in the
same process. A separate `hl.exe` is not needed.

Persona name is read from `rev.ini` `[steamclient] PlayerName=` (same key RevEmu
used). Identity for the ticket is `COMPUTERNAME` plus the C: volume serial.

## Build

32-bit MSVC.

Loader + steamclient:

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

Result: `build\cstrike.exe` and `build\steamclient.dll`. Copy them into the game folder yourself.

## Run

```bat
cstrike.exe
```

## License

MIT. See [LICENSE](LICENSE).
