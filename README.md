# Tekken Tag Tournament 2 Community Edition

An unofficial Windows community project for running *Tekken Tag Tournament 2* through a native static recompilation. Version 2.1 focuses on a cleaner launcher, configurable local controls, and standalone ranked-service foundations. It is not affiliated with Bandai Namco, Microsoft, or Valve.

## Current features

- **Launcher settings:** Configure display mode, output resolution, render scale, input polling (250, 500, 750, or 1000 Hz), save location, and shader options. The launcher repaints its window through a buffered paint pass, keeps the footer fixed, hides the native scrollbar, and supports mouse-wheel scrolling when a small or high-DPI window needs more room.
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

Use 64-bit Windows 10 or 11, a Direct3D 11-capable graphics device, and about 13 GB of free space. You must provide a legally obtained Xbox 360 disc image and Title Update 3. The installer processes those inputs locally and installs the launcher beside the runtime. The launcher can configure settings, open the save folder or log, and start the game.

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
