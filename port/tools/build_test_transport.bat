@echo off
setlocal
rem Run from an x64 Native Tools Command Prompt for VS 2022 (v143).
if not defined VCToolsInstallDir (
  echo Open an x64 Native Tools Command Prompt for VS 2022 first.
  exit /b 2
)
set ROOT=%~dp0..
set OUT=%ROOT%\build-transport-test
if not exist "%OUT%" mkdir "%OUT%"
set INC=/I"%ROOT%\third_party\enet\include" /I"%ROOT%\src\runtime\kernel"
set FLAGS=/nologo /W4 /WX /DWIN32_LEAN_AND_MEAN /D_WINSOCK_DEPRECATED_NO_WARNINGS /D_CRT_SECURE_NO_WARNINGS %INC%
for %%S in (callbacks compress host list packet peer protocol win32) do (
  cl %FLAGS% /wd4245 /wd5287 /wd4100 /wd4701 /c "%ROOT%\third_party\enet\%%S.c" /Fo"%OUT%\%%S.obj" || exit /b 1
)
cl %FLAGS% /std:c++17 /EHsc "%ROOT%\tools\test_transport_harness.cpp" "%OUT%\callbacks.obj" "%OUT%\compress.obj" "%OUT%\host.obj" "%OUT%\list.obj" "%OUT%\packet.obj" "%OUT%\peer.obj" "%OUT%\protocol.obj" "%OUT%\win32.obj" ws2_32.lib winmm.lib /Fe:"%OUT%\ttt2_test_transport.exe" || exit /b 1
"%OUT%\ttt2_test_transport.exe"
if errorlevel 1 exit /b 1
cl %FLAGS% /std:c++17 /EHsc "%ROOT%\tools\http_client.cpp" "%ROOT%\tools\test_integration_flow.cpp" "%OUT%\callbacks.obj" "%OUT%\compress.obj" "%OUT%\host.obj" "%OUT%\list.obj" "%OUT%\packet.obj" "%OUT%\peer.obj" "%OUT%\protocol.obj" "%OUT%\win32.obj" ws2_32.lib winmm.lib winhttp.lib /Fe:"%OUT%\test_integration_flow.exe" || exit /b 1
python "%ROOT%\tools\run_integration_flow.py" "%OUT%\test_integration_flow.exe"
exit /b %errorlevel%
