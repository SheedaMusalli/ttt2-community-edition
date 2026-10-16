# Tekken Tag Tournament 2 Community Edition

An experimental Windows port of *Tekken Tag Tournament 2* built from a static recompilation
of the Xbox 360 release. This repository contains the launcher, installer source, runtime
integration, and development tools for the community edition. It is an unofficial project and
is not affiliated with Bandai Namco, Microsoft, or Valve.

## What works in this build

- The installer processes a user-provided Xbox 360 disc image and Title Update 3 locally.
- The pre-launch menu can configure display mode, output resolution, internal render scale,
  shader compilation preferences, renderer selection, save location, and keyboard bindings.
- Save data can be backed up and restored from the launcher.
- The launcher's Online page can check a ranked-service server, read a provisional profile,
  join or cancel a queue, and refresh queue status. Its bearer token is protected with Windows
  DPAPI for the current Windows account.
- A Python standard-library matchmaking and provisional rating service is included for local
  testing or self-hosting.

The launcher service panel is not connected to the game's in-match session calls. A queue
reservation does not start a fight. Steam matchmaking, FSR, and DLSS are not available in this
build. Treat the ranked ratings as a prototype: results are peer-reported and are not protected
against modified or colluding clients.

## Install and play

Use Windows 10 or 11, 64-bit, with a DirectX 11-capable graphics device and about 13 GB of free
space. You must provide a legally obtained Xbox 360 disc image of the game and its Title Update
3 package. The installer extracts and processes those inputs on your PC. It installs the
launcher beside the runtime and creates Desktop and Start menu shortcuts to the launcher by
default. The launcher can be used to configure settings, open the log or save folder, and start
the game.

The installer and runtime are unsigned community builds. Review the source and build locally
if you do not want to run a binary obtained from another person.

## Build from source

Building the game runtime requires the original dump and update inputs, the XenonRecomp source
tree and generated files, and a Windows x64 C++ toolchain. This workspace was built with
Visual Studio 2022 Build Tools, the v143 libraries, and Clang-CL; native MSVC cannot compile
some generated PowerPC translation code. Start with [BUILDING.md](BUILDING.md) for the build
layout and toolchain details, and [port/INSTALLER.md](port/INSTALLER.md) for installer inputs
and behavior.

Run the ranked service tests with Python 3.10 or newer:

```powershell
python -m unittest discover -s port/tools -p test_ranked_service.py -v
```

The service can be started locally after provisioning test accounts:

```powershell
python port/tools/ranked_service.py --db ranked.sqlite3 provision player_one
python port/tools/ranked_service.py --db ranked.sqlite3 serve
```

The `provision` command prints a bearer token once. Keep it private. For a second test account,
run `provision` with another player ID. The launcher defaults to `http://127.0.0.1:8765`; it
requires HTTPS when connecting to a non-local server. See [deploy/README.md](deploy/README.md)
for the experimental Docker Compose setup.

## Online status and project boundaries

The standalone service manages matchmaking reservations and provisional ratings only. It does
not carry gameplay traffic or arbitrate the game's real ranked results. Full in-game online play
still needs verified guest session data, a runtime adapter, gameplay transport, and testing
between two separate clients. This build does not impersonate Xbox Live or bypass game-license
checks. App ID 480 is rejected; Steam support requires an authorized app ID and SDK.

See [RANKED_FOUNDATIONS.md](RANKED_FOUNDATIONS.md) for the inspected integration boundaries,
test results, deployment notes, and remaining blockers. Do not treat this prototype as a
production competitive service.

## Rights and redistribution

The installer embeds a recompiled game executable, and this repository does not declare a
license that grants redistribution rights for the full project. Do not redistribute the
installer or game-derived files unless you have confirmed the necessary rights. Third-party
components retain their own licenses; see their accompanying notices and source trees.
