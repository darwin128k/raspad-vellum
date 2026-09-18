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
- **Auth** — 64-byte Vellum ticket (`VLLM`). qproto classifies it as `CA_VELLUM`, on purpose not RevEmu / SC2009 / OldRevEmu. Servers without Vellum support will reject the ticket.
- **Identity** — persona is the OS user name. The ticket ident is hostname plus a machine serial (Windows: `COMPUTERNAME` and the C: volume serial; Linux: `gethostname` and `/etc/machine-id`).
- **Server browser** — favorites/history via `config/serverbrowser.vdf`. Internet lists come from `config/master.vdf` (HTTP JSON catalogs and/or UDP A2M hosts). LAN is UDP broadcast. Game/map/ping are filled with A2S.
- **HTTP** — real `ISteamHTTP` for FastDL and list fetches (WinINet on Windows, libcurl on Linux).
- **Voice** — `ISteamUser` capture/encode/decode. Microphone opens only while `+voicerecord` is held. Packets are Steam Voice Opus (24 kHz), so other Vellum/Steam-format clients can hear you. No extra voice plugin.

## Config

Written next to the game / `steamclient` binary.

**`config/master.vdf`** — Internet tab sources. Missing file gets a default HTTP catalog. `http://` / `https://` is JSON (`{offset}` for paging). Anything else is a UDP master: hostname or IPv4, optional `:port` (default `27011`). Several entries are queried together; IPs are deduped.

```
"master"
{
	"1"
	{
		"address"		"https://api.gamemonitoring.net/servers?game=10&limit=100&offset={offset}"
	}
}
```

**`config/serverbrowser.vdf`** — favorites and history, same layout as stock GoldSrc.

## Build

32-bit only, to match `hw` and `steam_api`.

### Windows

MSVC x86. Opus is fetched and linked statically.

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

### Linux

32-bit g++ (`g++-multilib`). Needs system Opus (`<opus/opus.h>`) and ALSA for voice.

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

## License

MIT. See [LICENSE](LICENSE).

Windows builds vendor [Opus](https://github.com/xiph/opus) (BSD-style) through CMake FetchContent. Linux builds against the distro Opus package.
