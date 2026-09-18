# Vellum

GoldSrc loader and steamclient for the Raspad stack. Two CMake targets, Windows
and Linux:

- **loader** (`cstrike.exe` / `cstrike_linux`) — stand in for Steam, then launch the
  engine.
- **steamclient** (`steamclient.dll` / `steamclient.so`) —
  `CreateInterface` / `SteamInternal_CreateInterface` for `SteamClient012` (8684)
  and `SteamClient020` (Oct 2024 GoldSrc), plus a 64-byte Vellum auth blob. qproto
  classifies that blob as `CA_VELLUM`, distinct from RevEmu / SC2009 / OldRevEmu.

No `rev.ini`, no `steam.dll`, no extra sidecar DLLs. Copy `cstrike.exe` and
`steamclient.dll` next to `hl.exe` and run `cstrike.exe`. One loader instance at
a time (CD-ROM style). Optional `-launch` / `-appid` on the command line.

Does not know about mods, filesystem, or `hw`. That lives in
[raspad-hl](https://github.com/darwin128k/raspad-hl): mutex, `FileSystem_Stdio`,
`hw.dll` / `hw.so`, `IEngineAPI::Run`, optional `-dll` and MetaHook. Vellum
either spawns that `hl` binary (loader) or compiles `src/launcher.cpp` from
`HL_DIR` into this process (standalone).

## Modes

**Loader (default).** Steam environment in this process, then run `hl.exe -game cstrike`
(Linux: `./hl_linux -game cstrike`). The loader pid stays the "live Steam".

- Windows: named mapping/event, registry `ActiveProcess`, `CreateProcess`.
- Linux: `SteamAppId`, a private `$HOME/.steam/sdk32` that points at our
  `steamclient.so` (SteamAPI_Init ignores `LD_LIBRARY_PATH` and would otherwise
  load Steam's copy), Steam Runtime i386 libs for CEF/GTK, then `exec`.

**Standalone.** Same Steam, then `HlLauncher_Run` from raspad-hl in the same
process. A separate `hl` is not needed. MetaHook embed is Windows-only.

Persona name is the OS user name. Identity for the ticket is hostname plus a
machine serial (`COMPUTERNAME` and C: volume serial on Windows; `gethostname`
and `/etc/machine-id` on Linux).

## Build

### Windows

32-bit MSVC.

```bat
build.bat
```

Standalone (`HL_DIR` defaults to `..\raspad-hl` if present, else `..\hl`):

```bat
build.bat standalone
build.bat standalone D:\src\raspad-hl
```

Result: `build\cstrike.exe` and `build\steamclient.dll`. Copy them into the game
folder yourself.

### Linux

32-bit g++ (`g++-multilib`).

```sh
./build.sh
./build.sh standalone
./build.sh standalone ../raspad-hl
```

Result: `build-linux/cstrike_linux` and `build-linux/steamclient.so`. Copy both next to
`hw.so`. Loader mode also needs `hl` in that folder.

## Run

Windows:

```bat
cstrike.exe
```

Linux:

```sh
chmod +x cstrike_linux
./cstrike_linux
```

## License

MIT. See [LICENSE](LICENSE).
