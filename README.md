# Vellum

GoldSrc loader and `steamclient` for the Raspad stack. It stands in for Steam so Counter-Strike 1.6 (and other Half-Life 1 games) can run without the Steam client, RevEmu sidecars, or extra plugins.

Two targets, Windows and Linux:

| Target | Output | Role |
|---|---|---|
| **loader** | `cstrike.exe` / `cstrike_linux` | Pretend to be Steam, then start the engine |
| **steamclient** | `steamclient.dll` / `steamclient.so` | Steamworks facades the engine actually calls |

No `rev.ini`, no `steam.dll`, no MetaHook/MetaVoice/MetaAudio. Copy the two binaries next to `hl.exe` / `hl_linux` and run the loader.

The engine, filesystem, and `hw` live in [raspad-hl](https://github.com/darwin128k/raspad-hl). Vellum either spawns that `hl` binary (default loader mode) or compiles `src/launcher.cpp` from `HL_DIR` into this process (standalone).

## What steamclient implements

- **SteamClient012 / 017 / 020** — enough of `CreateInterface` for GoldSrc 8684 and Steam Half-Life (Oct 2024 / 10210+).
- **Auth** — by default a 64-byte Vellum ticket (`VLLM`, `CA_VELLUM`). Optional `-DVELLUM_AUTH_REVEMU2013=ON` emits a RevEmu 2013 blob instead (`CA_REVEMU2013` on Reunion). One or the other, not both.
- **Identity** — persona is the OS user name. The ticket ident is hostname plus a machine serial (Windows: `COMPUTERNAME` and the C: volume serial; Linux: `gethostname` and `/etc/machine-id`).
- **Server browser** — favorites/history via `config/serverbrowser.vdf`. Internet lists come from `config/masterserver.vdf` (HTTP JSON catalogs and/or UDP A2M hosts). LAN is UDP broadcast. Game/map/ping are filled with A2S.
- **HTTP** — real `ISteamHTTP` for FastDL and list fetches (WinINet on Windows, libcurl on Linux).
- **Voice** — `ISteamUser` capture/encode/decode. Microphone opens only while `+voicerecord` is held. Packets are Steam Voice Opus (24 kHz), so other Vellum/Steam-format clients can hear you. No extra voice plugin.

## Config

Written next to the game / `steamclient` binary.

**`config/masterserver.vdf`** — Internet tab sources. Missing file gets a default HTTP catalog. `http://` / `https://` is JSON (`{offset}` for paging). Query parameters belong to that address: GameMonitoring uses `status=1` and optional `country={country}` (Windows/OS region, not VPN IP; or write `country=RU` yourself). Drop `{country}` for a world list. JSON `status` / `country` are optional — missing means keep the server. Anything else is a UDP master: hostname or IPv4, optional `:port` (default `27011`). Several entries are queried together; IPs are deduped.

```
"master"
{
	"1"
	{
		"address"		"https://api.gamemonitoring.net/servers?game=10&status=1&country={country}&limit=500&offset={offset}"
	}
}
```

**`config/serverbrowser.vdf`** — favorites and history, same layout as stock GoldSrc.

## Build

32-bit only, to match `hw` and `steam_api`.

### Windows

Needs:

- Visual Studio 2022 with the **Desktop development with C++** workload and the **MSVC x86** toolset (`vcvars32.bat` — `build.bat` looks under Enterprise by default)
- [CMake](https://cmake.org/) 3.16+
- [Ninja](https://ninja-build.org/) on `PATH`
- Git (CMake FetchContent clones Opus)

Opus is fetched and linked statically. WinINet / WinMM come with the Windows SDK.

```bat
build.bat
build.bat debug
```

Standalone (optional `HL_DIR`, default `..\raspad-hl` or `..\hl`):

```bat
build.bat standalone
build.bat standalone D:\src\raspad-hl
```

Output: `build\cstrike.exe`, `build\steamclient.dll`. Copy both into the game folder.

### Linux / WSL

Needs a 32-bit toolchain plus **Opus** and **libasound2** (ALSA) for voice. On Ubuntu/Debian (including WSL):

```sh
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install cmake g++ gcc-multilib g++-multilib \
    libopus-dev:i386 libasound2-dev:i386
```

| Package | For |
|---|---|
| cmake, g++, gcc-multilib, g++-multilib | 32-bit C++ (`-m32`) |
| **libopus-dev:i386** | Opus (`<opus/opus.h>`, `libopus`) |
| **libasound2-dev:i386** | ALSA / libasound2 (`alsa/asoundlib.h`) |

To **run** the game you also need **libcurl4:i386** (`libcurl.so.4`). steamclient loads it with `dlopen`, so the binary still starts without it — but HTTP FastDL and HTTP master lists do nothing. Connect, voice, LAN, and UDP masters keep working. Curl is not required to compile.

```sh
sudo apt install libcurl4:i386
```

```sh
./build.sh
./build.sh standalone
./build.sh standalone ../raspad-hl
```

Output: `build-linux/cstrike_linux`, `build-linux/steamclient.so`. Copy both next to `hw.so`. Loader mode also needs `hl` in that folder.

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

One loader instance at a time. Optional `-launch` / `-appid`. Default game is `cstrike`.

Release builds write no `vellum.log`. Debug (`build.bat debug`) logs a timestamped session: identity, interfaces, connect ticket, master list, and voice tx/rx.

CMake `-DVELLUM_NO_SERVER_BROWSER=ON` skips `config/masterserver.vdf` so the Internet tab stays empty. Default is **OFF** (search enabled). Favorites and LAN are unchanged. This is for a later one-server build that will not ship a public browser.

CMake `-DVELLUM_AUTH_REVEMU2013=ON` (or `build.bat revemu2013`) sends RevEmu 2013 tickets so Reunion/DProto servers that allow `cid_RevEmu2013` will accept the client. Default is **OFF**: VLLM only, no RevEmu dialect. The two tickets are not mixed in one build.

## License

MIT. See [LICENSE](LICENSE).

Windows builds vendor [Opus](https://github.com/xiph/opus) (BSD-style) through CMake FetchContent. Linux builds against the distro Opus package.
