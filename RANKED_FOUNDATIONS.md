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
- Ratings start at 1000, K=32. The service persists each match's participants, character
  teams, score, winner, start/end times, status, and both rating deltas. Only identical
  participant summaries update rating and wins/losses atomically; conflicting summaries
  are marked `disputed` with zero deltas. Aborted or mutually expired matches do not rate.
  These are **provisional community ratings**: two colluding or modified clients can
  fabricate agreement. The service's displayed Dan tier is a provisional rating-derived
  tier, not the game's official Dan progression or rank logic.
- A walkover after one participant becomes unresponsive awards a provisional win to the
  still-connected player, records the match as `disconnected`, increments the absent
  player's `disconnect_count`, and updates `disconnect_rate` as disconnects divided by
  completed rated games. The service cannot distinguish a malicious quit from a network
  failure, so these penalties remain provisional.
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
The v2.1 self-registration endpoint is enabled by default only for loopback-bound
servers; remote binds require `TTT2_RANKED_ALLOW_REGISTRATION=1`. For local development
the server binds to `127.0.0.1:8765`; `serve --host` and `serve --port` (also configurable
through `TTT2_RANKED_HOST` and `TTT2_RANKED_PORT`) allow container/server binding. Database
files are ignored by Git. Stop with Ctrl+C. No service is left running by tests.

Authenticated player operations use POST, JSON, and
`Authorization: Bearer <player-token>`. The leaderboard and history GET routes are public:

| Path | Body | Purpose |
|---|---|---|
| `/v1/profile` | `{}` | Own provisional rating and completed games |
| `/v1/queue` | `{"build":"tu3-v1","region":"asia"}` | Join/renew 30-second lease or receive a pending acceptance offer |
| `/v1/status` | `{}` | Current acceptance/reservation/active match, queued, cooldown or idle; does not renew lease |
| `/v1/matches/{id}/accept` | `{}` | Accept a pending match offer; both players must accept |
| `/v1/matches/{id}/decline` | `{}` | Decline the offer, restore the opponent to queue and start cooldown |
| `/v1/cancel` | `{}` | Remove queue entry; use abort for an existing match |
| `/v1/match` | `{"match_id":"..."}` | Participant-only match status including terminal state |
| `/v1/ready` | `{"match_id":"..."}` | Mark ready; both participants activate the match |
| `/v1/report` | `{"match_id":"...","winner":"alice"}` | Participant ID or `draw`; immutable, retry-safe |
| `/v1/matches/{match_id}/report` | `{"p1_chars":["Kazuya"],"p2_chars":["Jin"],"p1_score":3,"p2_score":1,"winner_id":"alice"}` | Submit full summary; both players must submit the same summary |
| `/v1/abort` | `{"match_id":"..."}` | End reservation/active match without rating change |
| `/v1/register` | `{"player":"player_name"}` | Create account and return bearer token (loopback by default; remote opt-in) |
| `/v1/leaderboard?limit=50` | — | Top players ordered by rating, wins, and stable ID |
| `/v1/players/{id}/history?limit=20` | — | Recent match ledger entries for a player |

Each match report requires one or two character identifiers per player, integer scores from
0 to 99, and a `winner_id` matching the score (`draw` requires equal scores). Reports are
immutable. Repeating the same report is safe; a conflicting report marks the match disputed
and does not change ratings. `disconnect_rate` is returned as a fraction from 0 to 1.

The launcher checks status every second and renews its 30-second queue lease every 10 seconds.
Other clients should also renew `/v1/queue` periodically while searching; `/v1/status` alone
does not renew the queue lease. After a challenger is found, both players must accept within
12 seconds before `/v1/status` returns connection details; retain the match ID to inspect the
final result. Build names are client-declared in this
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

### Rollback coordinator prototype

Added `port/src/runtime/kernel/net_rollback.{h,cpp}` and the standalone
`ttt2_test_rollback` CMake target. The coordinator has a 2–10 frame snapshot ring, bounded
local input delay (0–8 frames), previous-input prediction, late-input correction and replay,
snapshot checksums, and rejects conflicting duplicate inputs. The synthetic test compares the
replayed state byte-for-byte with a linear simulation and also checks delayed input and snapshot
metadata. Build and run it with:

```powershell
cmake -S port -B port/build-rollback-vs -G "Visual Studio 17 2022" -A x64
cmake --build port/build-rollback-vs --target ttt2_test_rollback --config Release
ctest --test-dir port/build-rollback-vs -C Release -R rollback_loop --output-on-failure
```

Validated on this host using MSVC v143: target compiled with `/W4 /WX` and printed
`PASS: rollback loop, prediction, bounded snapshots, checksum, and input delay`; CTest passed.
This is a coordinator fixture only. No live game rollback is enabled. The runtime does not yet
expose a verified deterministic snapshot boundary or a single-threaded replayable game-step
callback: guest execution and GPU/vblank/interrupt work span runtime threads, and the full guest
address space contains non-simulation state. Capturing/restoring guessed ranges or replaying the
existing interrupt path could corrupt the game or duplicate side effects. Therefore no calls were
added to `xsession.cpp`, `xlive.cpp`, or the guest frame path. Online session discovery and packet
transport remain separate from this rollback prototype; RTT/ping and rollback HUD telemetry are
not wired. The next integration step requires identifying simulation-owned mutable memory and
separating deterministic logic ticks from rendering, audio, host callbacks and asynchronous I/O,
then validating that boundary against captured local runs.

### Synchronized input timeline prototype

Added `port/src/runtime/kernel/netplay_input.{h,cpp}` and the standalone
`ttt2_test_netplay_input` CMake/CTest target. It encodes versioned controller samples with frame
IDs, assigns the local input delay, bounds input history, and exposes a nonblocking gate that
returns a frame only after both local and remote samples exist. MSVC v143 built the target with
`/W4 /WX`; its packet, timing, stale-input and gate checks pass. This is not connected to game
input or a live peer: `XamInputGetState` has no frame ID, the 60 Hz vblank callback is not proven
to be the game's once-per-frame simulation step, and the transport receiver is currently owned
by the virtual socket layer. Blocking either path or treating guest user 1 as the network
opponent without confirming the title's online input flow could deadlock the game or misroute
controls. No `xsession.cpp` or `xlive.cpp` hook was added. See
[`NETPLAY_INPUT_PHASE1.md`](NETPLAY_INPUT_PHASE1.md) for the boundary audit and memory inventory.

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
   The new coordinator test does not prove deterministic game-state simulation or enable rollback.
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

### Ranked match acceptance lifecycle (2026-10-17)

Pairing now enters `pending_acceptance` for 12 seconds. The service records each participant's
acceptance independently and returns the opponent's player ID, provisional rating and disconnect
rate, plus the remaining time. It does not return the peer endpoint, role, match token or session
key until both participants have accepted and the state changes to `confirmed`. This prevents a
client from using a reservation before the other player has agreed.

Authenticated service routes:

| Route | Behavior |
|---|---|
| `GET /v1/status` | Read the caller's pending prompt, confirmed match, queue, cooldown or idle state. |
| `POST /v1/matches/{id}/accept` | Record the caller's acceptance; the second acceptance confirms the match. An expired offer returns HTTP 408 after applying its timeout/requeue rules. |
| `POST /v1/matches/{id}/decline` | End the pending offer, restore the other player to the queue using the original queue priority, and apply a 30-second cooldown to the decliner. |

When only one client accepts and the countdown expires, that client is restored to the front of
the queue and the non-accepting client receives a 30-second cooldown. If neither accepts, both
receive the cooldown. The SQLite schema migration adds acceptance flags, deadlines, preserved
queue timestamps and per-player cooldown timestamps to existing databases. Existing accepted
matches retain their IDs and secrets in the database; secrets are exposed through status only for
confirmed or legacy reserved/active matches.

The launcher polls status asynchronously while matchmaking is active. Its “Challenger Found”
dialog shows the opponent summary and countdown, supports accept/decline, locks the page controls,
and sends a decline if the dialog is closed. The panel is repositioned on window resize. Both
accepting changes the launcher state to confirmed, but does not start the game: the runtime's
ranked session handoff is not implemented or validated. The acceptance feature is therefore a
service/launcher lifecycle, not playable online matchmaking.

Verification: `python -m unittest discover -s port/tools -p test_ranked_service.py -v` passes all
31 service tests, including migration, pending-secret redaction, both accept paths, decline,
timeout/requeue, cooldown and authenticated HTTP routes. The updated `ranked_e2e` harness now
accepts both clients before reading the endpoint and session key; this local two-process harness
still verifies signaling/transport only, not the game itself.

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

### XNet peer socket route verification (2026-10-17)

Audited `xsession.cpp`, `xlive.cpp`, `xsocket.cpp`, `matchmaking.cpp` and `net_transport.cpp`. The socket shim maps XNet virtual addresses to transport peer IDs, sends game datagrams through `net::Send`, and queues received datagrams for `NetDll_recvfrom` and `select`. This is a host-side implementation; guest ABI behavior still requires validation against a generated game build.

### Ranked endpoint signaling and ENet adapter (2026-10-17)

The ranked service accepts an optional syntax-checked IPv4 `listen_endpoint`. Paired clients receive roles, the opponent's advertised endpoint, and a shared random 16-byte session key. The endpoint remains a client claim and does not prove reachability, NAT traversal, or peer identity.

`net_transport.cpp` uses vendored ENet for numeric IPv4 peers, with sequenced-unreliable input and reliable ordered control channels. The runtime adapter is opt-in through `TTT2_RANKED_RUNTIME=1` and requires `TTT2_RANKED_SERVICE`, `TTT2_RANKED_TOKEN`, `TTT2_RANKED_PLAYER`, and for remote play a reachable `TTT2_RANKED_ADVERTISE_IP`. It registers the reservation endpoint and copies match/peer/session values into the host's current `XSESSION_INFO` representation. No Steamworks SDK, Spacewar identity, relay, public address discovery, or production matchmaking security is enabled. The guest session ABI and in-game flow remain unverified, so this is not a claim of playable online ranked mode.

### XSessionWriteStats payload diagnostics (2026-10-17)

XGI message `0x000B0025` now emits a read-only hex dump of at most the first 256 bytes of its incoming guest parameter buffer, then returns the same success status (`0`). The dump includes a byte offset, hex bytes, printable ASCII, Windows caller thread ID, `GetTickCount64()` milliseconds, and the runtime's presented-frame count. The frame value counts successfully presented frames since process start; it is not a title simulation-frame or match-state identifier. Invalid guest ranges are reported without dereferencing them. No guest memory is modified.

The dispatcher call contract available in this runtime is `XMsgStartIORequest` PPC registers `r3=app`, `r4=message`, `r5=overlapped`, `r6=guest buffer`, and `r7=buffer length`; this capture applies when `app=0xFB` and `message=0x000B0025`. For `XMsgStartIORequestEx`, the same first five arguments are used by the shared handler. The handler receives `r6`/`r7` as `buffer`/`length`. The XGI message's inner `XSessionWriteStats` structure layout and any offsets within its payload are not yet known: the logger deliberately dumps the supplied block as opaque bytes and does not infer nested pointers or property IDs. No live game match capture has been validated in this development environment.

### Native build and test results (2026-10-17)

Using the existing Visual Studio 2022 Build Tools CMake/Ninja/Clang-CL and v143 SDK, `tekken_recomp.exe`, `ttt2_setup.exe`, `ttt2_setup_probe.exe`, `ttt2_test_ranked_e2e.exe`, and `ttt2_test_xnet_route.exe` were built. CMake needed `VSINSTALLDIR` set to the Build Tools root so it could locate the CRT payload. The first compile found and fixed a Clang-CL string-copy error in `ranked_adapter.cpp` and a string concatenation error in the E2E test.

The Python ranked-service suite passes all 16 tests. CTest `ranked_e2e` passes: two local processes acquire a reservation over HTTP, authenticate with ENet and exchange bidirectional messages. CTest `xnet_route` builds but fails at the child-process shutdown assertion: the server process does not exit before the harness timeout. That shutdown issue remains open. Successful compilation and the isolated E2E test do not prove a live game match works.

The installer includes the rebuilt host game runtime and launcher resources, but still requires a user's legally obtained disc image and Title Update 3 to create installed game data.

### UDP relay and RTT-based relay allocation (2026-10-17)

Added the standalone `ttt2_relay_daemon` CMake target under `port/tools/relay_daemon`. It exposes an admin-token protected HTTP registration API on loopback by default and routes the 17-byte UDP envelope (16-byte random relay token plus one-byte player role) between the first observed endpoint for each side. Sessions are keyed by the relay token, associated with a match ID for registration/deletion, and expire after their configured TTL. Linux uses 32-message `recvmmsg`/`sendmmsg` batches and large socket buffers; Windows uses non-blocking WinSock readiness polling and `recvfrom`/`sendto`.

`ranked_service.py` now accepts optional per-node `client_rtt_map` values on queue submission, validates their range, chooses the node minimizing worst-player RTT (then total RTT), persists a separate random relay token, and includes the relay endpoint/token only in confirmed match responses. If `TTT2_RELAY_REGISTER_URL` and `TTT2_RELAY_ADMIN_TOKEN` are configured, or a node supplies its own `register_url` in `TTT2_RELAY_NODES`, the service registers that confirmed match with the selected relay daemon. Configure nodes with `TTT2_RELAY_NODES` JSON (`[{"id":"region-a","endpoint":"203.0.113.10:9100","register_url":"http://10.0.0.5:9101"}]`); for a single relay, `TTT2_RELAY_ENDPOINT` is accepted. Clients may provide their measured RTT map using `TTT2_RELAY_RTT_MAP`.

The Windows runtime now stores the confirmed relay endpoint and token, keeps sending direct ENet traffic during a 500 ms connection window, and then routes the virtual-socket datagrams over the relay framing if direct ENet has not connected. The relay route is locked for that peer once fallback starts. This routing code is experimental: the relay forwards opaque UDP datagrams and does not itself guarantee confidentiality, abuse resistance, competitive integrity, symmetric-NAT reachability, or playable game-session integration. Session acceptance, guest ABI behavior, and live match start remain unverified. The local socket test proves relay forwarding only.

Verification on this Windows host: the Release relay daemon and runtime compile with Clang-CL/v143. `test_relay_daemon.py` passes registration, both forwarding directions, source-address translation, and teardown; CTest `relay_daemon` and `ranked_e2e` pass; the ranked-service suite passes 33 tests including RTT selection and input validation. Linux execution was not verified because WSL is not installed on this host.

Build commands used:

```powershell
$env:VSINSTALLDIR = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
cmake -S port -B port\build-latency
cmake --build port\build-latency --target tekken_recomp
cmake --build port\build-latency --target ttt2_setup ttt2_setup_probe
cmake --build port\build-latency --target ttt2_test_ranked_e2e ttt2_test_xnet_route
ctest --test-dir port\build-latency --output-on-failure -R "ranked_e2e"
```

### v2.1 launcher, registration, and package (2026-10-17)

The launcher now uses a memory-DC paint pass for its main window, suppresses the
native vertical scrollbar, forwards mouse-wheel input from child controls, and keeps
the fixed status/action footer outside the page viewport. Combo-box page layout uses
the control's collapsed height rather than its expanded drop-down height. Display
and performance settings were compacted; input polling is shown in a dedicated
section. Tall pages retain wheel-based scrolling and hide controls that would
otherwise cover the footer. This code compiles, but pixel-level review across DPI
scales and window sizes remains outstanding.

The Online page's **Generate Token** action creates a random suffix for the local
Windows username, POSTs to `/v1/register`, stores the returned bearer token through
the existing per-user DPAPI settings path, and validates it with `/v1/profile`.
The service rejects duplicate IDs, limits each source IP to five registrations per
hour in one process, and returns 403 when registration is disabled. The supplied
Compose deployment enables forwarded-address parsing only behind its private Caddy
proxy. Loopback-bound services allow registration by default; non-loopback services require the explicit
`TTT2_RANKED_ALLOW_REGISTRATION=1` setting. This in-memory limit is not distributed
abuse protection; public deployment still needs operational safeguards.

Validation on this host: `python -m unittest discover -s port/tools -p
test_ranked_service.py -v` passes 18 tests. CMake configured with Visual Studio 2022
Build Tools, Clang-CL and v143 libraries; the Release `ttt2_setup` target compiled
successfully after correcting a Win32 `LONG`/C++ integer mismatch in the paint code.
The setup embeds the matching launcher and locally generated game runtime. These
game-derived executables remain local artifacts and are not added to Git. The
installer can process a legally obtained disc image and TU3 locally.

The verified clean Release build uses the Windows SDK `rc.exe` for VERSIONINFO
resources instead of Clang-CL's RC emulation. The installer and generated runtime
both report FileVersion/ProductVersion `2.1.0`. The default CMake selection prefers
the installed Windows SDK resource compiler when a caller has not explicitly chosen
one.

The v2.1 source documentation records the launcher repaint/layout changes, token
automation and networking boundary. Guest ABI and in-game session behavior remain
unverified; standalone service and transport tests do not establish playable online
ranked matches.

The full CTest suite was also rerun after the v2.1 build: `rollback_loop`,
`netplay_input`, and `ranked_e2e` pass. `xnet_route` still fails consistently at its
child-process completion stage (`server exit 4` after 10 seconds); the harness does
not complete the expected socket datagram round-trip, so XNet route integration is
not validated and remains an open blocker.

### v2.2.0 source update (2026-10-17)

The launcher adds a configurable console-log toggle and clears the launcher canvas
through the buffered repaint path to prevent stale text and overlapping controls. The
runtime keeps installed diagnostics in the local log file when the console window is
hidden. The performance overlay's visibility and toggle binding remain configurable.
These UI changes have been compiled; visual behavior can vary with Windows DPI and
display scaling.

The ranked service records match participants, character teams, scores, winner,
timestamps, terminal status and rating deltas. It exposes public leaderboard and player
history queries, accepts mutually verified reports, records disputes without rating
changes, and tracks provisional disconnect walkovers. Match offers require both clients
to accept before connection details are disclosed. The launcher can hand a confirmed
reservation to the runtime through child-process environment values, avoiding a second
queue request. Result capture through XGI `0x000B0025` remains read-only and opaque; the
authentic nested stats layout has not been identified.

The runtime includes an experimental IPv4 ENet path and optional UDP relay fallback.
The C++17 relay daemon has a protected HTTP session-registration endpoint, TTL cleanup,
and platform-specific batch receive paths. The service can select a configured relay
from client RTT maps. Automated loopback tests cover service acceptance, ledger behavior,
reservation handoff, and relay datagram forwarding. These tests do not verify Tekken's
full in-game session ABI, public NAT traversal, or a playable remote ranked match.

The current build also retains the local save-container fix, launcher input/display
settings, Player 1/Player 2 keyboard controls, high-rate polling selections, shader-cache
warm-up, and selectable performance-overlay metrics. Polling selections above 1000 Hz
do not make the host API deliver new controller samples at those rates; the game still
runs on its approximately 60 Hz simulation loop. Performance changes are not a guarantee
that every stage or match scene will hold a fixed frame rate, and stage-wide graphical
validation remains incomplete.

Version metadata is set to `2.2.0` for the runtime and launcher build. The public GitHub
release is source-only: the repository's distribution policy excludes game-derived
executables and installers from public assets. GitHub-generated source archives remain
available for contributors to build locally with their own lawful game inputs.

### Save creation failure fix (2026-10-17)

The screenshots' “Failed to create save data” prompt was caused by the runtime being
compiled with `TTT2_LOCAL_SAVES=0`: `SaveCreateContent` then returned access denied
for all save-container creation requests. The CMake default is now enabled, and a
Windows regression target verifies create, mount, enumerate, reopen, and delete
against a temporary profile save root. The test passes under the installed
Clang-CL/v143 toolchain. A Release runtime and installer were rebuilt with this
setting enabled; fresh profiles store data under
`%LOCALAPPDATA%\TTT2Recomp\saves\<profile XUID>`.

### Virtual LAN transport and Netplay HUD (2026-10-18)

The replacement transport binds IPv4/ENet and UDP discovery to an active VPN/LAN
adapter. ONLINE offers a Virtual LAN / VPN mode, optional active local IPv4 override,
comma-separated peer targets (optionally IP:discovery-port), four-packet recovery and
HUD checkbox. PERFORMANCE also exposes the HUD toggle. F2 toggles it during play.
Settings use [Netplay]; obsolete [VPN] settings are discarded.

GetAdaptersAddresses selects the explicit local address, then Hamachi, Radmin,
Tailscale, other named VPN interfaces and LAN. Interface-up status does not prove VPN
membership or provider connectivity. Join the same private network in the VPN app.
The launcher does not install, authenticate or control VPN applications. Invalid local
addresses and malformed peer targets are rejected instead of falling back silently.

Create Player Match on the host and search Player Match on the client. If provider
broadcast does not work, enter the host VPN IPv4 on the client. Allow the game through
Windows Firewall for UDP discovery (29876 by default) and its logged ENet game port.
Discovery fans out to every explicit target plus subnet/global broadcast. A second
local instance uses the next free discovery port; use the host's logged IP:PORT on
the client. Records expire after four seconds; withdrawal removes closed sessions.
Per-endpoint peer IDs and distinct local sign-in identities prevent same-PC aliasing.
The retired World Tekken Federation service is separate and is not restored here.

Environment: TTT2_NET=hamachi (or lan), TTT2_LOCAL_IP, TTT2_PEER_IP,
TTT2_DISCOVERY_PORT, TTT2_NET_ROLLBACK=1 and TTT2_NET_HUD=1. Runtime --local-ip= and
--peer-ip= switches are also accepted. Confirmed ranked reservations use the separate
coordinator transport. Both LAN peers need this build; the protocol is unauthenticated
and intended for trusted private networks.

Small datagrams (at most 512 bytes) use a versioned, network-order TRBK envelope with
four consecutive opaque packets, epoch/sequence metadata and 16-bit QPC timestamp/echo
fields. The decoder validates lengths before changing state, recovers up to three
missing packets in order, suppresses duplicates/reordering and counts gaps beyond the
window. Larger datagrams, stream/control messages and discovery beacons are untouched.
VDP bytes remain opaque: no unverified two-byte prefix is stripped.

The bounded lock-free atomic telemetry collector reports adapter, endpoint, measured
smoothed echo RTT/jitter, packet recovery/loss totals and 120 history samples. The native
D3D11 HUD composites a persistent texture from a cached GDI text/graph surface, refreshed
at most 60 times per second, below the performance panel or at top right. Green means
low RTT, amber recovery/high RTT and red a gap beyond the window. SYNCED means valid
transport delivery, not deterministic simulation synchronization. RTT stays N/A before
an echo arrives. Counters are labelled as packets rather than fabricated game frames.

Packet recovery is not deterministic simulation rollback. There is no verified guest
frame-input schema or deterministic state save/restore hook. ReportSimulationSync is
an integration hook without a connected simulation-tick source; Frame-Rift shows N/A
and the pacing function adds no artificial sleep until a verified tick source calls it.
Simulation rollback and frame-rift pacing therefore remain incomplete.

Verification covers the Release runtime/launcher, packet loss/recovery, deduplication,
malformed input, stream epochs, payload bounds, timestamp rollover, concurrent telemetry,
a rendered HUD fixture and two independent processes exchanging session beacons, actual
ENet messages, broadcast datagrams and session withdrawal. Existing XNet route and
ranked integration tests pass. These tests do not certify native in-game sessions on
two physical PCs or any VPN provider. Actual gameplay and frame-time profiling remain
necessary; zero frame-time impact is not claimed.

The current runtime does not expose verified attack startup, hit/block stun, frame advantage, or a
reliable game-mode gate for practice versus live online rounds. It also has no verified hook for
the end-of-match rematch transition. Therefore Tekken 8-style frame data panels and the requested
online post-match action menu remain unimplemented; `XSessionWriteStats` is still treated as an
opaque diagnostic payload rather than decoded using invented fields.


### v2.3.0 release update (2026-10-18)

Launcher settings now put Virtual LAN transport on its own VPN LAN tab. The performance overlay is narrower, its graph has an independent setting, and both overlays can be positioned in a selected screen corner. The runtime and installer version metadata is v2.3.0. The release package includes the installer and TU3. VPN multiplayer remains experimental; the release does not claim two-PC gameplay verification or deterministic game-simulation rollback.
