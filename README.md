# Vellum

GoldSrc loader and steamclient for the Raspad stack. Two CMake targets, Windows
and Linux:

- **loader** (`cstrike.exe` / `cstrike`) — stand in for Steam, then launch the
  engine.
- **steamclient** (`steamclient.dll` / `steamclient.so`) —
  `CreateInterface("SteamClient012")` and a 64-byte Vellum auth blob. qproto
  classifies that blob as `CA_VELLUM`, distinct from RevEmu / SC2009 / OldRevEmu.

Does not know about mods, filesystem, or `hw`. That lives in
[raspad-hl](https://github.com/darwin128k/raspad-hl): mutex, `FileSystem_Stdio`,
`hw.dll` / `hw.so`, `IEngineAPI::Run`, optional `-dll` and MetaHook. Vellum
either spawns that `hl` binary (loader) or compiles `src/launcher.cpp` from
`HL_DIR` into this process (standalone).

## Modes

**Loader (default).** Steam environment in this process, then run `ProcName`
from `rev.ini`. The loader pid stays the "live Steam".

- Windows: named mapping/event, registry `ActiveProcess`, `LoadLibrary(steam.dll)`,
  `CreateProcess` on `ProcName` (usually `hl.exe -game cstrike`).
- Linux: `SteamAppId`, `LD_LIBRARY_PATH` so `steam_api.so` finds our
  `steamclient.so`, `~/.steam/steam.pid` if Steam is not already running, then
  `exec` `ProcName` (default `./hl -game cstrike`).

**Standalone.** Same Steam, then `HlLauncher_Run` from raspad-hl in the same
process. A separate `hl` is not needed. MetaHook embed is Windows-only.

Persona name is read from `rev.ini` `[steamclient] PlayerName=`. Identity for the
ticket is hostname plus a machine serial (`COMPUTERNAME` and C: volume serial on
Windows; `gethostname` and `/etc/machine-id` on Linux).

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

Result: `build-linux/cstrike` and `build-linux/steamclient.so`. Copy both next to
`hw.so`. Loader mode also needs `hl` from raspad-hl in that folder. Optional
`rev.ini`:

```ini
[Loader]
ProcName=./hl -game cstrike
[steamclient]
PlayerName=YourName
```

If `ProcName` is omitted, the Linux loader defaults to `./hl -game cstrike`.

## Run

Windows:

```bat
cstrike.exe
```

Linux:

```sh
chmod +x cstrike
./cstrike
```

## License

MIT. See [LICENSE](LICENSE).
