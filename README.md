# Tekken Tag Tournament 2 Community Edition

An unofficial Windows community project for running *Tekken Tag Tournament 2* through a native static recompilation. Version 2.3.0 adds configurable compact overlays and experimental Virtual LAN telemetry alongside the launcher, performance diagnostics, local save support, and experimental ranked-service and relay foundations. It is not affiliated with Bandai Namco, Microsoft, or Valve.

## Current features

- **Launcher settings:** Configure display mode, output resolution, render scale, input polling (250, 500, 750, 1000, 2000, 4000, or 8000 Hz), save location, and shader options. The launcher repaints its window through a buffered paint pass, keeps the footer fixed, hides the native scrollbar, and supports mouse-wheel scrolling when a small or high-DPI window needs more room.
- **Keyboard and controller assignment:** Click a binding and press a key to assign it. Player 1 and Player 2 each have D-pad, face, shoulder, Start/Back, LS click, RS click, and LS/RS directional bindings. Choose automatic assignment, keyboard P1/controller P2, controller P1/keyboard P2, or shared P1 input. The in-game keyboard setup reads and edits Player 1's saved bindings.
- **Local two-player keyboard play:** Enable **Player 2 keyboard** in Controls to publish a separate second keyboard device alongside Player 1. Player 2 has its own default layout and can be rebound independently. The runtime exposes a connected second local device (keyboard or controller) as a distinct signed-in guest user so the game can recognize two local players; VS Battle and Pair Play still need in-game validation. Spacebar has no hidden Start/menu action; bind it explicitly if desired.
- **Performance overlay:** Choose FPS, frame-time graph, CPU/GPU utilization, system and process memory, GPU memory, and CPU/GPU temperature metrics. Turn the overlay on or off in the launcher and assign a toggle key (F10 by default). Temperature readings can be unavailable when Windows or the hardware does not expose a supported sensor.
- **Console logging:** Choose whether the game opens its diagnostic console at launch. Installed builds continue to write runtime diagnostics to the per-user log file, so turning off the console only hides the extra window.
- **Shader cache warm-up:** Previously cached D3D11 shader variants are prepared before the first game draw. The warm-up window reports progress, known variant count, and an ETA. A shader variant the game has never encountered cannot be compiled in advance; it is compiled when first discovered and can still cause a hitch.
- **Graphics path:** The runtime provides Direct3D 11 hardware rendering and a software fallback. D3D12, Vulkan, FSR, and DLSS are not implemented. GPU rendering accelerates supported rasterization and pixel-shader work, while guest command processing and some vertex work still use the CPU. The CPU fallback worker pool is capped by default to reserve scheduling capacity for the game and driver; `TTT2_RASTER_WORKERS` is available for profiling overrides.
- **Save tools:** Choose a save folder and create or restore local save backups from the launcher.
- **Game save data:** The Release build enables the local save-container backend by default. New profiles can create and use save data under `%LOCALAPPDATA%\TTT2Recomp\saves`; the launcher can open that location and manage backups.
- **Ranked-service prototype:** The launcher can contact a self-hosted service, create a player identity, save the bearer token encrypted with Windows DPAPI, validate the token, request a profile, enter or cancel a queue, and refresh status. When a challenger is found, both players must accept within 12 seconds. After confirmation, the launcher retrieves the reservation and starts the game with the match ID, session key, role, opponent endpoint, and optional relay details. The runtime imports that confirmed reservation directly and registers its ENet peer without queueing again. The SQLite ledger records mutually verified match summaries, character teams, scores, results, timestamps, and rating changes; public endpoints expose a leaderboard and recent player history. Disconnect walkovers update provisional rating and disconnect statistics. Local loopback servers allow registration by default. Remote deployments keep it disabled unless the operator explicitly enables it; the service applies a basic per-IP registration limit.
- **Ranked transport experiments:** A standalone C++ relay daemon supports session registration, expiry, and two-way UDP forwarding. The service can select a configured relay using both clients' reported RTT maps; the runtime tries direct ENet first and can route through the relay after its connection window. These are transport foundations and do not establish working in-game online matches.
- **Runtime diagnostics:** XGI `XSessionWriteStats` message `0x000B0025` can be captured as a bounded, read-only hex dump with thread/time/frame context. The payload remains opaque; no guessed stat IDs are interpreted.
- **Frame data and post-match flow:** Tekken 8-style attack/advantage panels and a rematch/results menu are not implemented yet. The recompiled runtime has no verified source for per-move startup, hit/block advantage, or the title's online match-end transition, so it cannot produce accurate frame data or safely replace the game's existing result flow yet.

## Important limitations

The confirmed launcher handoff and IPv4/ENet peer registration are implemented and exercised in a two-process loopback test. That does not establish that Tekken's full online session state machine or fight protocol works between real players: guest ABI integration, match lifecycle, NAT behavior, and two-machine game testing remain unverified. The original World Tekken Federation service has been retired; its “service has not started” message is separate from the community ranked service. Ratings are based on client reports rather than trusted game results. This project does not spoof Xbox Live or bypass game-license checks. App ID 480 is rejected; Steam integration requires an authorized app ID and SDK.

The game loop currently runs at approximately 60 simulation updates per second. A high-refresh display can improve scanout, but does not make the game produce 180 unique simulation frames per second. Frame-time drops have been reported across multiple stages; the local shader parity tools can test captured renderer programs, but do not constitute a complete visual audit of every stage or guarantee that all frame drops are fixed.

## Version 2.3.0

This release includes the launcher console visibility setting and repaint cleanup, runtime diagnostics, ranked acceptance and confirmed-session handoff, match ledger and profile endpoints, IPv4/ENet transport work, and the experimental UDP relay daemon with RTT-based relay selection. It also carries the current save-container, input-polling, overlay, and performance-path changes. The experimental Virtual LAN transport binds an active VPN/LAN adapter, supports explicit peer targets and session beacons, and adds four-packet recovery plus the F2 Netplay HUD. Native in-game matchmaking across two PCs remains unverified; packet recovery is not deterministic simulation rollback. See [RANKED_FOUNDATIONS.md](RANKED_FOUNDATIONS.md) for implementation status and test coverage. The v2.3.0 GitHub release includes the installer, TU3 update, and complete package archive.

## Install and play

Use 64-bit Windows 10 or 11, a Direct3D 11-capable graphics device, and about 13 GB of free space. Installation requires an Xbox 360 disc image you are legally entitled to use. This repository does not provide a game ISO download. The installer processes the supplied files locally; it does not download or include the game image.

The setup compresses its included runtime with Windows LZMS to reduce the download; installation expands the executable locally, so its installed size is unchanged.

### Installation steps

1. Download `ttt2_installer.exe` from the v2.3.0 release.
2. Download the TU3 asset from the v2.3.0 release. Keep the original asset filename if possible.
3. Prepare your legally obtained Xbox 360 *Tekken Tag Tournament 2* disc image (`.iso`) Download from [Google Drive](https://drive.google.com/file/d/1iNoqpuJu038nMPSJgDBgPupH_q3KfGpU/view?usp=drive_link). Do not rename the TU3 package to `.iso` or select it as the disc image.
4. Run `ttt2_installer.exe`. If Windows asks for administrator access, review the publisher and prompt before continuing; these community builds are unsigned.
5. In the installer, browse to the game ISO, browse to the TU3 package (or a folder containing the correct TU3 package), and choose an installation folder with about 13 GB of available space.
6. Start installation and allow the installer to verify the base game and update, build the installed game files locally, and copy the launcher and runtime into the selected folder.
7. When installation completes, start **TTT2 Launcher** from the created shortcut or the installation folder. Use **Check install** if the launcher reports that required files are missing, then choose **Launch Game**.
8. On the first game launch, create the game's save data when prompted. The default save location is under `%LOCALAPPDATA%\TTT2Recomp\saves`; you can change the location and manage backups in the launcher.

The installer accepts the retail ISO layouts documented in [port/INSTALLER.md](port/INSTALLER.md) and checks that the selected update is TU3. Installation failures or cancellations remove files written by that incomplete install. The launcher, game settings, logs, shader cache, and saves are kept in their documented local locations.

### Recommended starting settings

These are general baseline settings for a first run. They are not a benchmarked best configuration for every PC, and they cannot guarantee a fixed frame rate across every stage.

| Launcher option | Starting point | When to adjust |
|---|---|---|
| Renderer | **Direct3D 11 hardware renderer: On** | Turn it off only to diagnose graphics compatibility problems; this selects the slower software fallback. |
| Window mode | **Borderless** | Choose Fullscreen or Windowed to suit your display and capture setup. |
| Output resolution | **Your display's native resolution** | Lower it if you prefer reduced rendering load; this does not change the game's approximately 60 Hz simulation cadence. |
| Internal render scale | **Auto (follows resolution)** | Use 1.0x as a fixed baseline. Higher scales render more pixels and can increase GPU load. |
| Compile new shaders in the background | **On** | Turn off for troubleshooting if a shader or effect appears late or incorrectly. |
| Prepare saved shaders while the game starts | **On** | This warms previously cached variants; newly encountered shaders can still compile during play. |
| Fast shader compilation during a match | **On** | Turn off to prioritize compatibility if shader results appear incorrect. |
| Input polling | **500 Hz** | Use 250 Hz to reduce polling work or 750/1000 Hz for more frequent sampling. The 2000/4000/8000 Hz selections are retained for compatibility but host sampling is capped at 1000 Hz because Windows/XInput do not provide newer controller states at those rates. Higher rates do not change the game's simulation rate. |
| Performance overlay | **FPS and frame-time graph; F10 toggle** | Enable additional CPU, GPU, memory, or temperature fields while diagnosing performance. Temperature fields depend on available Windows sensor readings. |
| Player 2 keyboard | **Off unless playing local two-player** | Enable it in Controls to use a second, independently configurable keyboard layout. |

For troubleshooting, first keep the hardware renderer on, use Auto or 1.0x render scale, and watch the frame-time graph while testing the same scene. Change one setting at a time so you can tell whether it helped. Shader warm-up only prepares cached variants and is not a complete precompile of every shader in the game.

The installer and runtime are unsigned community builds. Review the source and build locally if you do not want to run binaries from another person. Do not redistribute game-derived files unless you have confirmed the necessary rights.

## Build from source

The game runtime build requires the original dump and update inputs, the XenonRecomp source and generated files, and a Windows x64 C++ toolchain. This project has been built with Visual Studio 2022 Build Tools, v143 libraries, and Clang-CL; native MSVC cannot compile some generated PowerPC translation code. See [BUILDING.md](BUILDING.md) for setup and [port/INSTALLER.md](port/INSTALLER.md) for installer inputs.

Run the ranked-service unit tests with Python 3.10 or newer:

```powershell
python -m unittest discover -s port/tools -p test_ranked_service.py -v
```

Run the input-polling harness:

```powershell
python port/tools/test_input_polling.py
```

Start a local service for testing. The launcher Online page can register a new identity and save its bearer token automatically:

```powershell
python port/tools/ranked_service.py --db ranked.sqlite3 serve
```

The launcher defaults to `http://127.0.0.1:8765` and requires HTTPS for non-local servers. The local service accepts self-registration by default; a remote service requires an operator to set `TTT2_RANKED_ALLOW_REGISTRATION=1` and should sit behind HTTPS. See [deploy/README.md](deploy/README.md) for the experimental Docker Compose setup and [RANKED_FOUNDATIONS.md](RANKED_FOUNDATIONS.md) for current runtime integration limits.

### Experimental UDP relay foundation

The source includes a standalone C++17 UDP relay daemon, `ttt2_relay_daemon`, for NAT-fallback experiments. On Windows it uses non-blocking WinSock; on Linux it uses batched `recvmmsg`/`sendmmsg`. The ranked service can select a configured relay node using both clients' optional `client_rtt_map` in `/v1/queue`. Relay credentials are returned only after both players accept the reservation. This transport path is experimental and does not make ranked matches playable by itself.

Build the daemon with CMake's default `BUILD_RELAY_DAEMON=ON`. For a local test, set `TTT2_RELAY_ADMIN_TOKEN` to a private random value and run `ttt2_relay_daemon --udp-port 9100 --http-port 9101`. Bind the HTTP registration API to loopback by default. Configure the ranked service with `TTT2_RELAY_NODES` as JSON, for example `[ {"id":"local","endpoint":"127.0.0.1:9100","register_url":"http://127.0.0.1:9101"} ]`, plus the same admin token. `TTT2_RELAY_REGISTER_URL` can provide a shared registration URL for a single relay. Public deployments need a private service-to-relay network, firewall rules, and a separate public UDP endpoint. Run the local round-trip check with `python port/tools/test_relay_daemon.py <path-to-ttt2_relay_daemon>`.

## Contributing

Contributions are welcome, especially reproducible performance captures, build fixes, test coverage, and work on the standalone ranked-service boundary. Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening an issue or pull request. The game-session boundary needs lawful local game inputs and runtime telemetry before it can be integrated safely. Remove personal tokens from logs before sharing them.

See [RANKED_FOUNDATIONS.md](RANKED_FOUNDATIONS.md) for the service architecture, verification results, deployment notes, and remaining blockers.

## Rights and third-party code

This repository does not declare a license that grants redistribution rights for the full game-derived project. Do not redistribute the installer or game-derived files unless you have confirmed the necessary rights. Third-party components retain their own licenses; see their included notices and source trees.
