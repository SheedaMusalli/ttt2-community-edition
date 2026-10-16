# Ranked multiplayer foundations

## Status

This branch implements a local standalone matchmaking/rating prototype and removes the
runtime's automatic Spacewar startup. **It does not enable playable online ranked mode.**
The Python service is not wired into the game's C++ session layer, and carries no gameplay
packets. Existing upstream player-match claims have not been reproduced here.

Upstream baseline: `3ba28b2aa88d4b880b4e1ab71d5df6d15609d969` (tag `upstream-baseline`).
The community edition source is published in the `SheedaMusalli/ttt2-community-edition`
repository. The original upstream repository and its baseline remain separate. Local game
disc and update inputs, generated game files, and build outputs are excluded from Git; see
`.gitignore`. Check the repository's current release page separately from this source tree:
some builds may contain game-derived executable code and need rights review before public
redistribution.

## Inspection findings

| Component | Existing implementation | Ranked implications |
|---|---|---|
| `port/src/runtime/kernel/net_transport.cpp` | Steam packets or UDP bound to localhost | Local UDP labels a channel reliable but implements no retransmission; unsuitable as an Internet transport |
| `steam_net.cpp`, `steam_bridge.cpp` | SteamNetworkingMessages, lobbies, helper-process IPC | Upstream hardcodes AppID 480 and accepts incoming Steam sessions; ranked needs participant authorization |
| `matchmaking.cpp` | Steam lobby records or local files with heartbeat expiry | Discovery exists; no authoritative ranked reservation/rating service |
| `xsession.cpp`, `Search` | Filters selected game contexts, ignores title XLAST rules | No skill-based selection; missing requested contexts can currently pass filtering |
| `xsession.cpp`, `ArbitrationRegister` | Registers the local machine/user only | Remote participant roster needs validated guest ABI integration |
| `xsession.cpp`, `ReadStats` and dispatch | Reads return empty views; writes/flush return success without persisting results | No durable ranked stats; must map guest stats IDs before replacing these stubs |
| `xlive.cpp`, `xsocket.cpp` | Local compatibility dispatch and virtual sockets | These are integration boundaries, not proof that original protected services are available |

No top-level project license was found. Dependency license statements are not a grant
to distribute the entire project or game. Rights clearance remains unresolved.

## Changes

- Networking now defaults to localhost. Unknown `TTT2_NET` values fail rather than
  silently selecting another backend.
- Steam requires `TTT2_NET=steam` and `TTT2_STEAM_APP_ID` containing an explicitly
  configured decimal AppID. Zero, malformed values, overflow, and 480 are rejected.
  After initialization, the actual Steam AppID must match the configured one.
  Configuration is not evidence of authorization; only use an AppID assigned to a
  product you are permitted to operate. Historical upstream comments about Spacewar
  describe previous behavior, not a supported setup for this branch.
- `port/tools/ranked_service.py`: Python 3.10+ standard-library prototype, SQLite
  transactions, administrator-provisioned bearer credentials stored as SHA-256 hashes,
  one queue entry per player, exact build/region matching, rating distance <= 200,
  30-second queue leases and reservations, both-player readiness, 15-minute match
  deadline, cancellation/abort, immutable result reports and idempotent Elo updates.
- Ratings start at 1000, K=32. Only agreeing participant reports update both ratings
  atomically. Disagreements, disconnect expiry and aborted matches award no points.
  These are **provisional community ratings**: two colluding or modified clients can
  fabricate agreement. This is not authoritative anti-cheat or original Tekken rank logic.
- Active matches record per-player heartbeats. Clients should heartbeat every 25 seconds;
  after 75 seconds without the opponent heartbeat, a participant may submit `/v1/forfeit`.
  The service waits a further 30 seconds before awarding a forfeit, and cancels the claim
  if the accused player sends a heartbeat in that window. Conflicting claims become
  `disputed` without rating changes. If both players go silent, the match becomes
  `expired` after 105 seconds. Readiness reservations still expire after 30 seconds.

## Run and test

Run from this project's root in PowerShell:

```powershell
python -m unittest discover -s port/tools -p test_ranked_service.py -v
python port/tools/ranked_service.py --db ranked.sqlite3 provision alice
python port/tools/ranked_service.py --db ranked.sqlite3 provision bob
python port/tools/ranked_service.py --db ranked.sqlite3 serve
```

Provision prints each token once; save it privately. Duplicate player IDs are rejected.
There is no public registration endpoint. For local development the server binds to
`127.0.0.1:8765`; `serve --host` and `serve --port` (also configurable through
`TTT2_RANKED_HOST` and `TTT2_RANKED_PORT`) allow container/server binding. Database
files are ignored by Git. Stop with Ctrl+C. No service is left running by tests.

Every endpoint uses POST, JSON, and `Authorization: Bearer <player-token>`:

| Path | Body | Purpose |
|---|---|---|
| `/v1/profile` | `{}` | Own provisional rating and completed games |
| `/v1/queue` | `{"build":"tu3-v1","region":"asia"}` | Join/renew 30-second lease or receive reserved match |
| `/v1/status` | `{}` | Current reservation/active match, queued or idle; does not renew lease |
| `/v1/cancel` | `{}` | Remove queue entry; use abort for an existing match |
| `/v1/match` | `{"match_id":"..."}` | Participant-only match status including terminal state |
| `/v1/ready` | `{"match_id":"..."}` | Mark ready; both participants activate the match |
| `/v1/report` | `{"match_id":"...","winner":"alice"}` | Participant ID or `draw`; immutable, retry-safe |
| `/v1/abort` | `{"match_id":"..."}` | End reservation/active match without rating change |

Queue again every ~10 seconds while searching. Poll status to receive a reservation;
retain the match ID to inspect the final result. Build names are client-declared in this
prototype; a production adapter must derive a compatibility fingerprint from the actual
game version, runtime version and protocol. No endpoints or secrets are embedded in game files.

## Steamworks versus standalone

Valve assigns each application its own AppID. Legitimate Steam integration needs an
authorized application, Steamworks partner access, suitable SDK licensing and content
distribution rights. Valve's onboarding rules exclude content without adequate rights.
Owning a retail disc or configuring a non-480 number does not establish these rights.
No Steam service was contacted or tested by this implementation.

Sources checked during this task:
- [Steamworks applications](https://partner.steamgames.com/doc/store/application)
- [Steamworks API initialization](https://partner.steamgames.com/doc/sdk/api)
- [Steamworks onboarding](https://partner.steamgames.com/steamdirect/)

A standalone service removes the Steam account/AppID dependency for matchmaking. It
does not settle game-code or distribution rights and does not provide Internet transport
by itself. Recommended next transport evaluation: an established reliable UDP library,
with participant-bound credentials and owned relay infrastructure. Do not expose the
existing loopback UDP backend as a substitute for reliable Internet networking.

## Validation and blockers

### Standalone ENet transport harness

Vendored ENet from `lsalzman/enet` at commit
`5a9c537fd464b3c6d3c55e1d3bd47588faf71b42` under its upstream MIT license. Headers use
the standard `port/third_party/enet/include/enet/` layout. `net_transport.h` defines
sequenced-unreliable channel 0 and reliable-ordered channel 1, plus length-checked,
constant-time reservation token and player-ID comparison. The harness serializes and
parses a fixed 80-byte handshake from the received ENet packet; only a matching token,
player ID, magic, version and packet length authenticates a peer. Invalid peers are
disconnected before game data is accepted.

From an x64 Native Tools Command Prompt for Visual Studio 2022, run
`port\tools\build_test_transport.bat`. It compiles vendored ENet C files with `/W4 /WX`
(suppressing known upstream-only warnings for signed conversions, enum comparisons,
unused parameters and legacy address initialization), compiles the C++ harness with
`/std:c++17 /W4 /WX`, links `ws2_32.lib` and `winmm.lib`, then executes the binary.
Validated on this host: the executable built and exited 0 with
`ENet authentication, rogue rejection, unreliable input and reliable ordering passed`.
The loopback run rejected `TOKEN_INVALID_999`, accepted the reserved token, received
all 20 reliable state messages in exact order and processed fresh unreliable input
despite application-level input drops. This is a localhost library harness only; it
does not establish NAT traversal, relay service, cryptographic confidentiality, or
integration with the game runtime.

### HTTP-to-ENet integration flow

The service issues one random shared reservation token when it creates a match and returns
it only to authenticated participants from queue/status/match responses. `port/tools/http_client.*`
is a standalone WinHTTP JSON POST helper with a five-second timeout and Bearer authentication.
`test_integration_flow.cpp` queues Alice and Bob, polls the reservation, compares their
service-issued tokens, readies the match, passes that exact token through a real loopback
ENet handshake, and submits matching results. `run_integration_flow.py` provisions temporary
accounts and a temporary SQLite database, starts the local service, launches the C++ test,
then cleans up the service and database.

Run `port\tools\build_test_transport.bat` from an x64 VS 2022 Native Tools prompt. It builds
both executables with MSVC v143; the integration executable links `winhttp.lib`, `ws2_32.lib`
and `winmm.lib`. The complete HTTP matchmaking through ENet handshake and ranked-result flow
was verified with exit code 0 and output `HTTP matchmaking -> shared token -> ENet
authentication -> ranked result passed`. This is a local host harness, not integration with
the game's session layer. The prototype returns its reservation token as a bearer capability
over local HTTP and must not be exposed over an untrusted network.

Fourteen Python tests pass, including a real localhost HTTP exchange between two authenticated
clients, concurrent queue reservation, persistence after reopening, repeated reports,
disputes, heartbeat-based forfeits, stale-match expiry, aborts/draws and malformed/oversize
requests. Initial testing exposed an unclosed SQLite connection on Windows; it was fixed.

Visual Studio Build Tools 2022 with the v143 toolset is installed. `test_online_policy.cpp`
was compiled with MSVC `/std:c++20` and executed successfully, covering rejection of 480,
malformed values and overflow, and acceptance of a valid decimal AppID. The hardcoded
`C:\ttt2` checkout requirement was removed from `BUILDING.md`; CMake's VC runtime lookup
uses the active Visual Studio installation or toolchain. No absolute drive path remains in
those build files.

Remaining implementation milestones:

1. Build the baseline and branch with user-provided game inputs. Record guest session
   create/search/arbitration and stats payloads during local matches, then validate
   roster layout, title stat identifiers and end-of-match outcomes with fixtures.
2. Add a C++ client adapter at matchmaking/session boundaries. Map service reservations
   to XSESSION_INFO and transport identities, provide remote arbitration registrants,
   and make stats reads/writes reflect real durable outcomes. Do not synthesize success
   for unimplemented service operations.
3. Implement authenticated packet transport and relays/NAT handling. Test reliable
   ordering, packet loss, jitter, reconnect, timeouts, desync detection and guest timing.
   This task did not implement rollback or prove deterministic simulation.
4. Replace the local HTTP harness for public use: TLS, account verification/token
   rotation, rate limits, bounded workers, operational monitoring, database migrations,
   backups/retention and abuse controls. The current single-process HTTPServer and
   client-reported outcomes are deliberately only a local development harness.
5. Validate full matches between two separate machines, ranking persistence, tampered
   reports and interrupted connections. Resolve rights and hosting before distribution.

No protected-service bypass, entitlement spoofing, or changes to original service
authentication were added. Full online ranked multiplayer remains blocked on the game
adapter, gameplay transport, build inputs, two-machine validation and rights review.

### Server deployment package (2026-10-16)

Added `deploy/Dockerfile`, `deploy/compose.yml` and `deploy/Caddyfile` for a Linux
VPS deployment. Compose stores SQLite in a named persistent volume, keeps the
Python service private to the Compose network, and publishes only Caddy on ports
80/443 for automatic HTTPS. `GET /healthz` checks database availability. Follow
`deploy/README.md` for DNS, firewall, launch, account provisioning, and backup
steps. The HTTP server now uses request threads so simultaneous client polling
does not block all other clients. Python remains standard-library only.

Validation: the service unit suite now includes the health endpoint. The container
image has not been built on this Windows host unless Docker verification below
records otherwise. This package makes the provisional matchmaking service
deployable; it does not connect the game runtime to it or make peer-reported
outcomes authoritative. Keep it a test/community service until those gaps and
public abuse controls have been addressed.

### Windows installer build (2026-10-16)

With the locally supplied `dump\Default.xex` and Title Update 3 package
`TU_174Q22P_0000008000000.00000000000O2`, XenonRecomp analysis and source generation completed,
and the x64 runtime and installer built successfully. The standalone artifact is
`outputs\ttt2_setup.exe` (63,811,584 bytes, SHA-256
`72E456FEFFD93B32B06ABBA43258376293CC68434E73A1AEB756A7AC40A86113`). It is a Windows x64
PE executable built with Visual Studio 2022 Clang-CL and v143 libraries. The generated PPC
code uses Clang/GNU compiler extensions, so native MSVC `cl.exe` cannot compile those sources;
Clang-CL was used for the runtime and installer.

CMake stages the VC runtime DLLs beneath the build payload before compiling resources, avoiding
resource-compiler failures on Visual Studio paths containing spaces. The installer now carries
the Sheeda Musalli SM icon and dark navy, teal and amber visual theme. Its install and uninstall
dialogs, executable metadata, publisher label, default folder and shortcut use “TTT2 Community
Edition by Sheeda Musalli.” The unprivileged preview dialog was opened and visually checked.
The production installer was not run because it expects an original disc image (`.iso`) plus
TU3; the supplied game directory is extracted data, not that disc image. Steamworks SDK files
are absent, so Steam online support is disabled in this build. The ranked-service suite was
rerun and all 13 tests passed.

### Launcher ranked-service panel (2026-10-16)

Added an Online page to the launcher with backend health check, profile lookup, matchmaking
queue, status refresh and queue cancellation. It sends bearer-authenticated requests to the
existing `/v1` service and accepts HTTPS endpoints, allowing plain HTTP only for localhost.
The bearer token is stored encrypted with Windows DPAPI for the current Windows account.
Match reservation tokens are hidden from the response panel. This is a launcher-to-service
control panel only: it does not hand reservations to the game or establish a fight session.
The installer already embeds `TTT2 Launcher.exe`, installs it beside the game, and creates
Desktop/Start shortcuts to the launcher by default; installer copy now calls this out.

The distribution README no longer instructs players to use Spacewar. Valve's Steamworks docs
describe App ID 480 as the Spacewar example application; this project rejects it and the current
build has no Steamworks SDK. No fake Xbox Live sign-in or game-license bypass was added. FSR and
DLSS are not exposed as options because the runtime has no corresponding upscaler integration.

The updated installer (`outputs/ttt2_setup.exe`, 65,149,440 bytes, SHA-256
`D8B8A70DBC96616F12D0A007ACC02A4ECC0E70D3965B3C5F971B7DCDB83C82CB`) and launcher
(`outputs/TTT2 Launcher - Online Panel.exe`, 697,856 bytes, SHA-256
`F5C39BC89C74729B9E93503B6E23E15F4DC903CDBCF9F91991D64679DEB473D4`) were rebuilt with
Clang-CL and the v143 Windows SDK. The installer resource's embedded launcher matches the
standalone build byte for byte. A screenshot review found that the Online page's response box
and explanatory note overlapped the fixed footer, and the persistent footer hint collided with
the live status line. The response box is now shorter, both redundant notes are removed, and the
footer status has clear space.
