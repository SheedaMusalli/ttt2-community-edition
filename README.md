# Tekken Tag Tournament 2 Community Edition

An unofficial Windows community project for running *Tekken Tag Tournament 2* through a native static recompilation. Version 2.1 focuses on a cleaner launcher, configurable local controls, and standalone ranked-service foundations. It is not affiliated with Bandai Namco, Microsoft, or Valve.

## Current features

- **Launcher settings:** Configure display mode, output resolution, render scale, input polling (250, 500, 750, 1000, 2000, 4000, or 8000 Hz), save location, and shader options. The launcher repaints its window through a buffered paint pass, keeps the footer fixed, hides the native scrollbar, and supports mouse-wheel scrolling when a small or high-DPI window needs more room.
- **Keyboard controls:** Click a binding and press a key to assign it. Player 1 and Player 2 each have D-pad, face, shoulder, Start/Back, LS click, RS click, and LS/RS directional bindings. The in-game keyboard setup reads and edits Player 1's saved bindings.
- **Local two-player keyboard play:** Enable **Player 2 keyboard** in Controls to publish a separate second keyboard pad alongside Player 1. Player 2 has its own default layout and can be rebound independently. A physical controller remains supported. Spacebar has no hidden Start/menu action; bind it explicitly if desired.
- **Performance overlay:** Choose FPS, frame-time graph, CPU/GPU utilization, system and process memory, GPU memory, and CPU/GPU temperature metrics. Temperature readings can be unavailable when Windows or the hardware does not expose a supported sensor.
- **Shader cache warm-up:** Previously cached D3D11 shader variants are prepared before the first game draw. The warm-up window reports progress, known variant count, and an ETA. A shader variant the game has never encountered cannot be compiled in advance; it is compiled when first discovered and can still cause a hitch.
- **Graphics path:** The runtime provides Direct3D 11 hardware rendering and a software fallback. D3D12, Vulkan, FSR, and DLSS are not implemented. GPU rendering accelerates supported rasterization and pixel-shader work, while guest command processing and some vertex work still use the CPU.
- **Save tools:** Choose a save folder and create or restore local save backups from the launcher.
- **Game save data:** The Release build enables the local save-container backend by default. New profiles can create and use save data under `%LOCALAPPDATA%\TTT2Recomp\saves`; the launcher can open that location and manage backups.
- **Ranked-service prototype:** The launcher can contact a self-hosted service, create a player identity, save the returned bearer token encrypted with Windows DPAPI, validate the token, request a profile, enter or cancel a queue, and refresh status. Local loopback servers allow registration by default. Remote deployments keep it disabled unless the operator explicitly enables it; the service applies a basic per-IP registration limit. The Python standard-library service stores reservations and provisional ratings.

## Important limitations

The ranked service does not yet connect two game sessions or carry fight traffic. A queue reservation does not start an in-game match, and ratings are based on client reports rather than trusted game results. The current IPv4/ENet signaling and socket-routing code is host-side groundwork; guest ABI and in-game session integration remain unverified. Full ranked play still needs a verified game-session adapter and two-machine game testing. This project does not spoof Xbox Live or bypass game-license checks. App ID 480 is rejected; Steam integration requires an authorized app ID and SDK.

The game loop currently runs at approximately 60 simulation updates per second. A high-refresh display can improve scanout, but does not make the game produce 180 unique simulation frames per second. Reported stage-specific slowdowns have not been reproduced and validated across every stage; the overlay and logs are diagnostic tools, not a guarantee that all frame drops are fixed.

## Install and play

Use 64-bit Windows 10 or 11, a Direct3D 11-capable graphics device, and about 13 GB of free space. The installer needs an Xbox 360 disc image that you are legally entitled to use, plus Title Update 3 (TU3), available as a separate asset on the [Releases page](https://github.com/SheedaMusalli/ttt2-community-edition/releases/latest). This repository does not provide a game ISO download. The installer processes the supplied files locally; it does not download or include the game image.

The setup compresses its included runtime with Windows LZMS to reduce the download; installation expands the executable locally, so its installed size is unchanged.

### Installation steps

1. Download the latest `ttt2_setup.exe` installer from [Releases](https://github.com/SheedaMusalli/ttt2-community-edition/releases/latest).
2. Download the TU3 asset from that same release. Keep the original asset filename if possible.
3. Prepare your legally obtained Xbox 360 *Tekken Tag Tournament 2* disc image (`.iso`). Do not rename the TU3 package to `.iso` or select it as the disc image.
4. Run `ttt2_setup.exe`. If Windows asks for administrator access, review the publisher and prompt before continuing; these community builds are unsigned.
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
| Input polling | **500 Hz** | Use 250 Hz to reduce polling work or 750/1000 Hz for more frequent sampling. 2000/4000/8000 Hz are available for experimentation but add substantial CPU work and may not be sustained consistently under Windows scheduling. Higher rates do not change the game's simulation rate. |
| Performance overlay | **FPS and frame-time graph** | Temporarily enable CPU, GPU, memory, or temperature fields while diagnosing performance. Temperature fields depend on available Windows sensor readings. |
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

## Contributing

Contributions are welcome, especially reproducible performance captures, build fixes, test coverage, and work on the standalone ranked-service boundary. Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening an issue or pull request. The game-session boundary needs lawful local game inputs and runtime telemetry before it can be integrated safely. Remove personal tokens from logs before sharing them.

See [RANKED_FOUNDATIONS.md](RANKED_FOUNDATIONS.md) for the service architecture, verification results, deployment notes, and remaining blockers.

## Rights and third-party code

This repository does not declare a license that grants redistribution rights for the full game-derived project. Do not redistribute the installer or game-derived files unless you have confirmed the necessary rights. Third-party components retain their own licenses; see their included notices and source trees.
