# Tekken Tag Tournament 2 Community Edition

An unofficial Windows community project around a native static recompilation of the Xbox 360 version of *Tekken Tag Tournament 2*. This project is not affiliated with Bandai Namco, Microsoft, or Valve.

## Project overview

The launcher provides a pre-launch settings interface for display mode, output resolution, internal render scale, shader compilation, renderer selection, save location, and keyboard bindings. It can back up and restore local saves and start the installed game.

The installer expects a legally obtained Xbox 360 disc image and Title Update 3. Those game inputs are processed locally and are not included in this repository.

## Ranked service status

The included matchmaking service is an experimental standalone prototype. It can manage accounts, queues, reservations, heartbeats, forfeit claims, and provisional ratings. The launcher's service panel can connect to that backend, but a queue reservation does not connect two game clients or start an in-game match. The prototype does not carry gameplay traffic or arbitrate authoritative game results.

Steam matchmaking, FSR, and DLSS are not implemented. Full online play requires verified guest session data, a runtime adapter, gameplay transport, and testing between separate clients. The project does not impersonate Xbox Live or use Spacewar AppID 480.

## Build and install

The Windows installer processes the game inputs on the user's PC. Building the game runtime requires the original dump and update inputs, XenonRecomp-generated sources, and a Windows x64 C++ toolchain. Python backend tests use the standard library.

## Distribution

No game dump or Title Update is included. The installer embeds a game-derived executable. Do not publish or redistribute that installer or other game-derived files unless you have confirmed the required rights. Third-party components retain their own licenses.
