# Contributing

Thanks for helping make ranked play possible. Contributions should be focused, reproducible,
and honest about what has been tested. The current ranked service is a matchmaking and
provisional-rating prototype; it is not connected to the game's in-match session layer and
does not provide playable online fights.

## Where help is useful

- Improve or test the Python matchmaking service, reservation lifecycle, heartbeat and forfeit
  handling, and rating transactions.
- Improve the standalone transport contract and loopback harness, including Windows builds.
- Make launcher, installer, and runtime builds more reproducible; improve setup and diagnostics.
- Document verified runtime behavior and capture integration requirements from a lawful local
  game installation.
- Investigate the guest session/stat ABI using reproducible local telemetry before proposing
  runtime changes.

## Before opening a change

1. Search existing issues and pull requests so related work can be coordinated.
2. Describe the user-visible problem and the smallest change that addresses it.
3. Include tests or exact reproduction steps. Never include account tokens, private logs, or
   machine-specific secrets.
4. Run the relevant checks and report the command and result. For service changes, run:

   ```powershell
   python -m unittest discover -s port/tools -p test_ranked_service.py -v
   ```

5. Update `RANKED_FOUNDATIONS.md` or the relevant build/deployment documentation when behavior
   or verified status changes.

## Runtime integration boundary

Do not guess Xbox guest structure layouts, title stat/context IDs, or session call semantics.
Changes to `port/src/runtime/kernel/xsession.cpp` and `xlive.cpp` need evidence from generated
guest code and runtime telemetry, plus tests that demonstrate the exact ABI assumptions.
Keep AppID 480 rejection in place. Do not spoof another product's AppID, bypass authentication
or license checks, or present peer-reported results as anti-cheat protected.

## Files and rights

Do not submit disc images, Title Update packages, extracted or generated game files, game-derived
executables, private credentials, or other material you do not have rights to redistribute.
Use legally obtained local inputs for development and share only source changes you are
authorized to publish. Review third-party dependency notices when changing vendored code.

This repository currently has no project-wide license granting rights to use or redistribute
the complete project. A contribution is not a grant of rights to game content or third-party
materials. Please discuss licensing questions before relying on this repository in a product.

## Pull requests

Keep pull requests small enough to review. Explain the behavior before and after the change,
list validation performed, and call out any limitation that remains. A passing unit test does
not establish compatibility with the game runtime; distinguish prototype tests from live
two-client in-game verification.
