TEKKEN TAG TOURNAMENT 2 - native PC port
=========================================

This is a native Windows port of the Xbox 360 game, made by static recompilation. It ships no
game data: the installer built the game files on this PC from YOUR OWN copy of the game (the
Xbox 360 disc image plus Title Update 3). Do not share the installed "game" folder.

Windows 10 or 11, 64-bit. A DirectX 11 graphics card. Every runtime DLL that is not part of
Windows is already in this folder.


PLAYING
-------
Start "TTT2 Community Edition" from the desktop or the Start menu. The launcher opens first;
choose settings on the left, press SAVE to keep them, or PLAY to start the game.

An Xbox controller (or any XInput pad) is the intended way to play; whoever presses a button
first is player 1. The keyboard also works as player 1:

    Arrow keys      D-pad                 A / S / Z / X   A / B / X / Y buttons
    Enter or Space  START                 Q / W           left / right bumper
    Backspace       BACK

    Alt+Enter       windowed <-> borderless full screen

Window mode, output resolution, internal render scale, shader compilation options and save folder
are available in the launcher. Window mode and resolution can also be changed in the game's own
menus: OPTIONS > DISPLAY SETTINGS. EXIT on the main menu, or closing the window, quits.


SAVES, SETTINGS AND THE LOG
---------------------------
Everything the game writes goes to

    %LOCALAPPDATA%\TTT2Recomp\

(saves and profiles, settings.ini, the shader cache, and log.txt). Uninstalling keeps that
folder unless you tick "Also delete my saved games". If something goes wrong, log.txt from
there is the file to send.

The first start of a fight compiles graphics shaders, so the first few seconds can stutter;
later runs use the cache.


ONLINE STATUS
-------------
The launcher includes a ranked-service panel for checking a community backend, viewing a
provisional profile, entering matchmaking, checking queue status, and cancelling a search.
The standalone backend does not connect the game's in-game Xbox Live session flow to a fight,
so a queue reservation alone does not start an online match.

Steam networking is disabled unless a legally authorized Steamworks App ID and SDK are configured
for this build. App ID 480 is Valve's Spacewar example application and is not used by this port.
Do not create a steam_appid.txt for 480 or use the launcher to impersonate Xbox Live sign-in or
game licenses. The ranked-service panel uses its own bearer token; remote server URLs must use
HTTPS. The local default URL is http://127.0.0.1:8765.


UNINSTALLING
------------
Settings > Apps > Installed apps > "Tekken Tag Tournament 2 (PC port)", or run uninstall.exe in
the install folder.


LEGAL
-----
Tekken Tag Tournament 2 is (c) Bandai Namco Entertainment. This port is an unofficial fan
project, not affiliated with or endorsed by Bandai Namco or Microsoft. You must own the game.
Steamworks: steam_api64.dll is (c) Valve Corporation, redistributed under the Steamworks SDK
licence.
