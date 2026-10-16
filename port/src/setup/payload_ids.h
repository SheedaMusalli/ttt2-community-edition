// Resource ids shared between payload.rc and the installer sources.
#pragma once

#define IDI_TTT2SETUP            1

#define IDR_GAME_EXE           101
#define IDR_STEAM_API          102
#define IDR_MSVCP140           103
#define IDR_MSVCP140_ATOMIC    104
#define IDR_VCRUNTIME140       105
#define IDR_VCRUNTIME140_1     106
#define IDR_README             107
#define IDR_LAUNCHER_EXE       108

#define IDD_MAIN               200
#define IDC_ISO_PATH           201
#define IDC_ISO_BROWSE         202
#define IDC_DEST_PATH          203
#define IDC_DEST_BROWSE        204
#define IDC_SC_DESKTOP         205
#define IDC_SC_STARTMENU       206
#define IDC_STATUS             207
#define IDC_PROGRESS           208
#define IDC_INSTALL            209
#define IDC_INTRO              210
#define IDC_ISO_LABEL          211
#define IDC_DEST_LABEL         212
#define IDC_TU_LABEL           213
#define IDC_TU_PATH            214
#define IDC_TU_BROWSE          215
#define IDC_BRAND              216
#define IDC_BRAND_TAG          217
#define IDC_HEADER_TITLE       218
#define IDC_HEADER_SUBTITLE    219
#define IDC_INSTALL_TITLE      223
#define IDC_INSTALL_NOTE       224
#define IDC_SOURCE_BADGE       225
#define IDC_UPDATE_BADGE       226
#define IDC_DEST_BADGE         227
#define IDC_FOOTER              228
#define IDC_UN_BRAND            229
#define IDC_UN_TITLE            230
#define IDC_BRAND_ICON          231
#define IDC_UN_ICON             232

#define IDD_UNINSTALL          220
#define IDC_UN_TEXT            221
#define IDC_UN_KEEPSAVES       222

// NOTE: d3dcompiler_47.dll is deliberately NOT shipped: it is inbox on Windows 10 and 11 and is
// not redistributable (the Tom & Jerry installer's rule). The UCRT, d3d11, xinput and xaudio2
// are inbox too.
