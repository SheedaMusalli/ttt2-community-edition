# TTT2 Community Edition v2.1

This local package contains the Release installer, its matching standalone launcher, the built game runtime and required Visual C++ runtime files, plus the standard-library ranked matchmaking service tools. The launcher is installed alongside the game by the installer. It supports display/resolution/render-scale choices, shader settings, controller polling choices, editable Player 1/Player 2 keyboard bindings, local second-player keyboard enablement, save management, overlay metrics, and ranked-service account/matchmaking controls.

The Online page can create a player identity against a loopback service and save the returned bearer token encrypted to the current Windows account. For a hosted service, account registration must be explicitly enabled by its operator and accessed over HTTPS. The service is a matchmaking/rating prototype: its reservation data is not yet connected to the in-game session layer, and reported results are not authoritative.

The installer includes game-derived executable code produced from locally supplied inputs. Keep this package private unless you have confirmed distribution rights. Do not publish the installer or runtime binaries to the public GitHub release. The source and documentation may be published separately.

See `README.md`, `RANKED_FOUNDATIONS.md`, and `deploy/README.md` for build, test, service and integration limitations. Python tools have no third-party dependencies; `requirements.txt` is intentionally empty apart from an explanatory comment.
