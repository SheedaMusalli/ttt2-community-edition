// ttt2_setup.exe -- the installer for the Tekken Tag Tournament 2 PC port. Modeled on the Tom &
// Jerry port's installer (same dialogs, uninstaller guard and /silent test mode).
//
// IT SHIPS NO GAME DATA. The player supplies their own Xbox 360 disc image and Title Update 3, and
// everything that came off them is produced at install time:
//   - the disc's files (dataNNN.bin archives, nrfo.bin, ...), 26 archives LZX-expanded on the way;
//   - TU3's data100.bin (expanded) + data100.ofs into game\update (the game's "update:" device);
//   - game\tekken.img: the disc's Default.xex with TU3's Default.xexp applied, decrypted - checked
//     against the SHA-1 of the image the port's executable was recompiled from.
// The only files carried inside this executable are ours (the port's exe, optional Steamworks
// client library, the VC++ runtime, a readme) and an original icon.
//
// Install layout:
//     <Program Files>\Tekken Tag Tournament 2\        exe + runtime + game\ (read-only)
//     %LOCALAPPDATA%\TTT2Recomp\                       saves, profiles, settings, shader cache, log
// Uninstalling leaves %LOCALAPPDATA%\TTT2Recomp alone unless the player ticks the box.
#include "setup/payload_ids.h"
#include "setup/payload.h"
#include "setup/game_data.h"
#include "setup/stfs.h"
#include "setup/xdvdfs.h"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <objbase.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <cwctype>

#include <algorithm>
#include <string>
#include <vector>

using namespace ttt2::setup;

#ifndef TTT2_VERSION_STR
#define TTT2_VERSION_STR "0.0.0"
#endif
#define TTT2_WIDE2(s) L##s
#define TTT2_WIDE(s) TTT2_WIDE2(s)
#define TTT2_VERSION_W TTT2_WIDE(TTT2_VERSION_STR)

#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)

static const wchar_t kAppName[]      = L"TTT2 Community Edition by Sheeda Musalli";
static const wchar_t kFolderName[]   = L"TTT2 Community Edition";
static const wchar_t kRegKey[]       = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TTT2Recomp";
static const wchar_t kShortcut[]     = L"TTT2 Community Edition.lnk";
static const wchar_t kUninstallExe[] = L"uninstall.exe";
static const wchar_t kUserDataDir[]  = L"TTT2Recomp";      // under %LOCALAPPDATA%
static const wchar_t kImageName[]    = L"game\\tekken.img";

// ---------------------------------------------------------------- shared state

struct InstallJob {
    HWND         dlg = nullptr;
    std::wstring iso, tu, dest;
    bool         scDesktop = true, scStartMenu = true;
    volatile LONG cancel = 0;

    CRITICAL_SECTION lock;
    int          pct = 0;
    std::wstring status;
    bool         ok = false;
    std::wstring error;
    HANDLE       log = INVALID_HANDLE_VALUE;   // /silent only
    std::vector<std::wstring> written;         // files this run created, removed again on failure

    void Set(int p, const std::wstring& s) {
        EnterCriticalSection(&lock);
        pct = p; status = s;
        LeaveCriticalSection(&lock);
        if (dlg) {
            PostMessageW(dlg, WM_APP_PROGRESS, 0, 0);
        } else if (log != INVALID_HANDLE_VALUE) {
            std::wstring w = s + L"\r\n";
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string utf8((size_t)(n > 0 ? n : 0), '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &utf8[0], n, nullptr, nullptr);
            char head[16];
            int hn = _snprintf_s(head, sizeof head, _TRUNCATE, "[%3d%%] ", p);
            DWORD put = 0;
            if (hn > 0) WriteFile(log, head, (DWORD)hn, &put, nullptr);
            if (!utf8.empty()) WriteFile(log, utf8.data(), (DWORD)utf8.size(), &put, nullptr);
        }
    }
};

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ---------------------------------------------------------------- filesystem helpers

static bool MakeDirs(const std::wstring& dir) {
    for (size_t i = 3; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == L'\\') {
            std::wstring part = dir.substr(0, i);
            if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                if (GetFileAttributesW(part.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
            }
        }
    }
    return true;
}

static void UnLog(const wchar_t* fmt, ...);

// Is this folder one of our installs? The uninstaller deletes a whole tree, so it never takes a
// path on trust - not from its own location, and not from a registry value. Markers that never
// change: the game image the installer built and the port executable beside it.
static bool LooksLikeInstall(const std::wstring& dir) {
    if (dir.size() < 4) return false;
    return GetFileAttributesW((dir + L"\\" + kImageName).c_str()) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW((dir + L"\\" + kGameExeName).c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        UnLog(L"    FindFirstFile(\"%s\\*\") failed, err %lu", dir.c_str(), GetLastError());
        return;
    }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p);
        else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir.c_str());
}

static uint64_t DirSizeKb(const std::wstring& dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += DirSizeKb(p) * 1024ull;
        else total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total / 1024ull;
}

static std::wstring KnownDir(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) { out = p; CoTaskMemFree(p); }
    return out;
}

static bool MakeShortcut(const std::wstring& lnkPath, const std::wstring& target,
                         const std::wstring& workDir, const std::wstring& iconPath) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link)))
        return false;
    bool ok = false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetDescription(L"TTT2 Community Edition by Sheeda Musalli");
    if (!iconPath.empty()) link->SetIconLocation(iconPath.c_str(), 0);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(lnkPath.c_str(), TRUE));
        pf->Release();
    }
    link->Release();
    return ok;
}

static bool WriteBytes(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = size == 0 || WriteFile(h, data, (DWORD)size, &put, nullptr);
    CloseHandle(h);
    return ok && put == size;
}

// ---------------------------------------------------------------- the title update

// The player may point at the TU file or at the folder holding all three updates: pick the
// package of this game whose Default.xexp targets 0.0.3.2.
static bool OpenTitleUpdate(const std::wstring& path, StfsPackage& pkg, std::vector<uint8_t>& xexp, std::wstring& why) {
    std::vector<std::wstring> candidates;
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { why = L"The title update file was not found."; return false; }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) candidates.push_back(path + L"\\" + fd.cFileName); }
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    } else candidates.push_back(path);

    std::wstring seen;
    for (const std::wstring& c : candidates) {
        std::string err;
        if (!pkg.Open(c.c_str(), err)) continue;
        if (pkg.TitleId() != kTitleId) { pkg.Close(); continue; }
        if (!pkg.ReadFile("Default.xexp", xexp)) { pkg.Close(); continue; }
        const std::string version = XexpTargetVersion(xexp);
        if (version == "0.0.3.2") return true;
        seen = Widen(version);
        pkg.Close();
    }
    if (!seen.empty())
        why = L"That is the title update for game version " + seen + L". The port needs Title Update 3 "
              L"(version 0.0.3.2, file name ending in \"O2\").";
    else
        why = L"No Tekken Tag Tournament 2 title update was found there. Choose the Title Update 3 "
              L"file (TU_174Q22P_0000008000000.00000000000O2) or the folder holding it.";
    return false;
}

// ---------------------------------------------------------------- the install worker

static bool Fail(InstallJob* job, const std::wstring& why) {
    job->error = why;
    job->Set(job->pct, L"Failed: " + why);
    return false;
}

struct Planned { const IsoEntry* entry; uint64_t outBytes; bool expand; };

static bool RunInstall(InstallJob* job) {
    std::string err;
    std::wstring why;

    job->Set(1, L"Reading the disc image\x2026");
    XdvdfsImage img;
    if (!img.Open(job->iso.c_str(), err)) return Fail(job, Widen(err));

    // Identity: the base executable must be the retail one the port was built from - the title
    // update is a delta against it and the image check below would fail anyway.
    const IsoEntry* xexEntry = img.Find("Default.xex");
    std::vector<uint8_t> baseXex;
    if (!xexEntry || !img.ReadEntry(*xexEntry, baseXex))
        return Fail(job, L"That disc image is not Tekken Tag Tournament 2 (no Default.xex).");
    if (Sha1Hex(baseXex.data(), baseXex.size()) != kBaseXexSha1)
        return Fail(job, L"That disc image is not the Tekken Tag Tournament 2 (USA, Europe) retail disc this port "
                         L"was made for, or the image is damaged.");

    job->Set(2, L"Reading the title update\x2026");
    StfsPackage tu;
    std::vector<uint8_t> xexp, tuArchive, tuIndex;
    if (!OpenTitleUpdate(job->tu, tu, xexp, why)) return Fail(job, why);
    if (!tu.ReadFile("data100.bin", tuArchive) || !tu.ReadFile("data100.ofs", tuIndex))
        return Fail(job, L"The title update file is incomplete or damaged.");
    const uint64_t tuExpanded = RecordExpandedSize(tuIndex);
    if (tuArchive.size() < 16 || !IsLzxContainer(tuArchive.data()) || !tuExpanded)
        return Fail(job, L"The title update file is incomplete or damaged.");

    job->Set(3, L"Applying Title Update 3 to the game\x2026");
    std::vector<uint8_t> image;
    if (!BuildGameImage(baseXex, xexp, image, err)) return Fail(job, Widen(err));

    // What to install: every top-level file of the game partition except the avatar pack (the
    // dashboard's, not the game's). Compressed archives are expanded to the size nrfo.bin gives.
    const IsoEntry* nrfoEntry = img.Find("nrfo.bin");
    std::vector<uint8_t> nrfo;
    if (!nrfoEntry || !img.ReadEntry(*nrfoEntry, nrfo)) return Fail(job, L"The disc image is incomplete (no nrfo.bin).");
    std::vector<std::string> archives;
    for (const IsoEntry& e : img.Entries())
        if (!e.dir && e.path.find('\\') == std::string::npos && e.path.size() == 11 &&
            _strnicmp(e.path.c_str(), "data", 4) == 0 && _stricmp(e.path.c_str() + 7, ".bin") == 0)
            archives.push_back(e.path);
    std::sort(archives.begin(), archives.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    if (archives.size() != 42) return Fail(job, L"The disc image is incomplete (expected 42 game archives).");
    const auto sizes = ArchiveSizes(nrfo, archives);

    std::vector<Planned> plan;
    uint64_t total = image.size() + tuExpanded + tuIndex.size();
    for (const IsoEntry& e : img.Entries()) {
        if (e.dir || e.path.find('\\') != std::string::npos || _stricmp(e.path.c_str(), "AvatarAssetPack") == 0) continue;
        uint8_t head[4] = {};
        if (e.size >= 4 && !img.ReadAt(e.offset, head, 4)) return Fail(job, L"The disc image could not be read.");
        Planned p{ &e, e.size, false };
        const auto it = sizes.find(e.path);
        if (e.size >= 16 && IsLzxContainer(head)) {
            if (it == sizes.end() || !it->second) return Fail(job, L"The disc image does not match the expected game data.");
            p.outBytes = it->second;
            p.expand = true;
        }
        plan.push_back(p);
        total += p.outBytes;
    }

    ULARGE_INTEGER freeAvail = {};
    if (GetDiskFreeSpaceExW(job->dest.substr(0, 3).c_str(), &freeAvail, nullptr, nullptr)) {
        const uint64_t need = total + PayloadBytes() + (256ull << 20);
        if (freeAvail.QuadPart < need) {
            wchar_t msg[256];
            swprintf_s(msg, L"There is not enough free space on that drive. About %.1f GB is needed.", need / 1e9);
            return Fail(job, msg);
        }
    }

    job->Set(4, L"Creating the install folder\x2026");
    const std::wstring gameDir = job->dest + L"\\game";
    if (!MakeDirs(gameDir + L"\\update"))
        return Fail(job, L"Could not create the install folder. Try a different location.");
    if (!WritePayload(job->dest.c_str(), err, &job->written)) return Fail(job, Widen(err));

    // The game files: 5..96% of the bar, by bytes written.
    uint64_t done = 0;
    auto report = [&](const std::wstring& what, uint64_t extra) {
        if (InterlockedCompareExchange(&job->cancel, 0, 0)) return false;
        const int pct = 5 + (int)((done + extra) * 91 / (total ? total : 1));
        static uint64_t shown = 0;
        if (done + extra - shown > (16u << 20) || extra == 0) {
            shown = done + extra;
            job->Set(pct, what);
        }
        return true;
    };
    std::vector<uint8_t> buf(1u << 20);
    for (const Planned& p : plan) {
        const std::wstring name = Widen(p.entry->path), out = gameDir + L"\\" + name;
        job->written.push_back(out);
        if (p.expand) {
            const std::wstring what = L"Unpacking " + name + L"\x2026";
            const IsoEntry* e = p.entry;
            if (!ExpandContainer([&](uint64_t off, void* b, uint32_t n) { return img.ReadAt(e->offset + off, b, n); },
                                 e->size, p.outBytes, out, [&](uint64_t w) { return report(what, w); }, err))
                return Fail(job, err == "Installation cancelled." ? Widen(err) : L"Could not unpack " + name + L": " + Widen(err) + L".");
        } else {
            const std::wstring what = L"Copying " + name + L"\x2026";
            HANDLE oh = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (oh == INVALID_HANDLE_VALUE) return Fail(job, L"Could not write to the install folder.");
            uint64_t left = p.entry->size, at = 0;
            bool ok = true;
            while (left && ok) {
                const uint32_t n = (uint32_t)(std::min<uint64_t>)(left, buf.size());
                DWORD put = 0;
                ok = img.ReadAt(p.entry->offset + at, buf.data(), n) && WriteFile(oh, buf.data(), n, &put, nullptr) && put == n;
                at += n; left -= n;
                if (ok && !report(what, at)) { CloseHandle(oh); DeleteFileW(out.c_str()); return Fail(job, L"Installation cancelled."); }
            }
            CloseHandle(oh);
            if (!ok) { DeleteFileW(out.c_str()); return Fail(job, L"The disc image could not be read, or the disk is full."); }
        }
        done += p.outBytes;
    }

    job->Set(96, L"Installing Title Update 3 data\x2026");
    for (const wchar_t* f : { L"\\update\\data100.bin", L"\\update\\data100.ofs" }) job->written.push_back(gameDir + f);
    job->written.push_back(job->dest + L"\\" + kImageName);
    if (!ExpandContainer([&](uint64_t off, void* b, uint32_t n) {
                             if (off + n > tuArchive.size()) return false;
                             memcpy(b, tuArchive.data() + off, n);
                             return true;
                         }, tuArchive.size(), tuExpanded, gameDir + L"\\update\\data100.bin", {}, err))
        return Fail(job, L"Could not unpack the title update: " + Widen(err) + L".");
    if (!WriteBytes(gameDir + L"\\update\\data100.ofs", tuIndex.data(), tuIndex.size()) ||
        !WriteBytes(job->dest + L"\\" + kImageName, image.data(), image.size()))
        return Fail(job, L"Could not write to the install folder (the disk may be full).");

    job->Set(98, L"Creating shortcuts\x2026");
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring uninst = job->dest + L"\\" + kUninstallExe;
    CopyFileW(self, uninst.c_str(), FALSE);
    const std::wstring game = job->dest + L"\\" + kGameExeName;
    const std::wstring launcher = job->dest + L"\\" + kLauncherExeName;
    if (job->scDesktop) {
        std::wstring d = KnownDir(FOLDERID_Desktop);
        if (!d.empty()) MakeShortcut(d + L"\\" + kShortcut, launcher, job->dest, launcher);
    }
    if (job->scStartMenu) {
        std::wstring s = KnownDir(FOLDERID_CommonPrograms);
        if (s.empty()) s = KnownDir(FOLDERID_Programs);
        if (!s.empty()) MakeShortcut(s + L"\\" + kShortcut, launcher, job->dest, launcher);
    }

    job->Set(99, L"Registering with Add or remove programs\x2026");
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        auto S = [&](const wchar_t* n, const std::wstring& v) {
            RegSetValueExW(k, n, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        };
        auto D = [&](const wchar_t* n, DWORD v) { RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE*)&v, sizeof v); };
        S(L"DisplayName", kAppName);
        S(L"DisplayVersion", TTT2_VERSION_W);
        S(L"Publisher", L"Sheeda Musalli");
        S(L"InstallLocation", job->dest);
        S(L"DisplayIcon", game);
        S(L"UninstallString", L"\"" + uninst + L"\" /uninstall");
        S(L"QuietUninstallString", L"\"" + uninst + L"\" /uninstall /quiet");
        D(L"NoModify", 1);
        D(L"NoRepair", 1);
        D(L"EstimatedSize", (DWORD)DirSizeKb(job->dest));
        RegCloseKey(k);
    }

    job->ok = true;
    job->Set(100, L"Done.");
    return true;
}

// A failed or cancelled install takes back what it wrote (up to 13 GB), file by file - never a
// tree delete, since the folder may have held something else. Folders go only if left empty.
static bool Install(InstallJob* job) {
    if (RunInstall(job)) return true;
    for (const std::wstring& f : job->written) {
        SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(f.c_str());
    }
    RemoveDirectoryW((job->dest + L"\\game\\update").c_str());
    RemoveDirectoryW((job->dest + L"\\game").c_str());
    RemoveDirectoryW(job->dest.c_str());
    return false;
}

static DWORD WINAPI InstallThread(LPVOID param) {
    InstallJob* job = (InstallJob*)param;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Install(job);
    PostMessageW(job->dlg, WM_APP_DONE, 0, 0);
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------- pickers

static std::wstring PickFile(HWND owner, const wchar_t* title, const COMDLG_FILTERSPEC* filters, UINT count) {
    IFileOpenDialog* dlg = nullptr;
    std::wstring out;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg)))
        return out;
    dlg->SetFileTypes(count, filters);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

static std::wstring PickFolder(HWND owner) {
    IFileOpenDialog* dlg = nullptr;
    std::wstring out;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg)))
        return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose where to install the game");
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

// ---------------------------------------------------------------- install dialog

static InstallJob g_job;
static HANDLE     g_thread = nullptr;
static bool       g_running = false;
static bool       g_desktopShortcut = true;
static bool       g_startMenuShortcut = true;
static bool       g_removeUserData = false;

// Match the launcher's dark metallic surfaces, Tekken red/blue accents, and Segoe UI typography.
static const COLORREF kBodyColor = RGB(16, 21, 28);
static const COLORREF kHeaderColor = RGB(19, 26, 35);
static const COLORREF kAccentColor = RGB(218, 47, 60);
static const COLORREF kBlueColor = RGB(53, 158, 207);
static const COLORREF kTextColor = RGB(233, 240, 247);
static const COLORREF kMutedColor = RGB(145, 163, 181);
static const COLORREF kFieldColor = RGB(27, 36, 47);
static HBRUSH g_bodyBrush = nullptr;
static HBRUSH g_headerBrush = nullptr;
static HBRUSH g_fieldBrush = nullptr;
static HBRUSH g_accentBrush = nullptr;
static HBRUSH g_edgeBrush = nullptr;
static HFONT g_brandFont = nullptr;
static HFONT g_titleFont = nullptr;
static HFONT g_sectionFont = nullptr;
static HFONT g_captionFont = nullptr;

static void UseDarkTitleBar(HWND hwnd) {
    const BOOL enabled = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &enabled, sizeof(enabled));
    DwmSetWindowAttribute(hwnd, 19, &enabled, sizeof(enabled));
}

static LRESULT CALLBACK DarkEditSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR subclassId, DWORD_PTR reference) {
    if (msg == WM_NCPAINT) return 0;
    if (msg == WM_PAINT) {
        const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        HDC dc = GetDC(hwnd);
        if (dc) {
            RECT r{}; GetClientRect(hwnd, &r);
            HPEN edge = CreatePen(PS_SOLID, 1, RGB(43, 56, 70));
            HGDIOBJ oldPen = SelectObject(dc, edge);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(edge);
            ReleaseDC(hwnd, dc);
        }
        return result;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, DarkEditSubclass, subclassId);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void EnsureBrandTheme() {
    if (!g_bodyBrush) g_bodyBrush = CreateSolidBrush(kBodyColor);
    if (!g_headerBrush) g_headerBrush = CreateSolidBrush(kHeaderColor);
    if (!g_fieldBrush) g_fieldBrush = CreateSolidBrush(kFieldColor);
    if (!g_accentBrush) g_accentBrush = CreateSolidBrush(kAccentColor);
    if (!g_edgeBrush) g_edgeBrush = CreateSolidBrush(RGB(43, 56, 70));
}

static void ApplyBrandFonts(HWND dlg, bool uninstall) {
    if (!g_brandFont) g_brandFont = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Display");
    if (!g_titleFont) g_titleFont = CreateFontW(-25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Display");
    if (!g_sectionFont) g_sectionFont = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (!g_captionFont) g_captionFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    const int largeIds[] = { uninstall ? IDC_UN_BRAND : IDC_BRAND,
                             uninstall ? IDC_UN_TITLE : IDC_INSTALL_TITLE };
    SendDlgItemMessageW(dlg, largeIds[0], WM_SETFONT, (WPARAM)g_brandFont, TRUE);
    SendDlgItemMessageW(dlg, largeIds[1], WM_SETFONT, (WPARAM)g_titleFont, TRUE);
    if (!uninstall) {
        SendDlgItemMessageW(dlg, IDC_HEADER_TITLE, WM_SETFONT, (WPARAM)g_brandFont, TRUE);
        SendDlgItemMessageW(dlg, IDC_BRAND_TAG, WM_SETFONT, (WPARAM)g_captionFont, TRUE);
        SendDlgItemMessageW(dlg, IDC_HEADER_SUBTITLE, WM_SETFONT, (WPARAM)g_captionFont, TRUE);
        for (int id : { IDC_SOURCE_BADGE, IDC_UPDATE_BADGE, IDC_DEST_BADGE })
            SendDlgItemMessageW(dlg, id, WM_SETFONT, (WPARAM)g_sectionFont, TRUE);
        SendDlgItemMessageW(dlg, IDC_FOOTER, WM_SETFONT, (WPARAM)g_captionFont, TRUE);
        for (int id : { IDC_ISO_BROWSE, IDC_TU_BROWSE, IDC_DEST_BROWSE, IDC_INSTALL, IDCANCEL,
                        IDC_SC_DESKTOP, IDC_SC_STARTMENU, IDC_STATUS })
            SendDlgItemMessageW(dlg, id, WM_SETFONT, (WPARAM)g_captionFont, TRUE);
    } else {
        for (int id : { IDOK, IDCANCEL, IDC_UN_KEEPSAVES })
            SendDlgItemMessageW(dlg, id, WM_SETFONT, (WPARAM)g_captionFont, TRUE);
    }
}

static bool IsHeaderControl(int id, bool uninstall) {
    if (uninstall) return id == IDC_UN_BRAND || id == IDC_UN_TITLE;
    return id == IDC_BRAND || id == IDC_BRAND_TAG || id == IDC_HEADER_TITLE || id == IDC_HEADER_SUBTITLE;
}

static void PaintDialogBackground(HWND dlg, HDC dc, bool uninstall) {
    RECT client{};
    GetClientRect(dlg, &client);
    FillRect(dc, &client, g_bodyBrush);
    RECT header = client;
    // Dialog template coordinates are in dialog units; WM_ERASEBKGND uses physical pixels.
    header.bottom = uninstall ? 86 : 84;
    FillRect(dc, &header, g_headerBrush);
    RECT accent = header;
    accent.left = 0;
    accent.top = header.bottom - 2;
    FillRect(dc, &accent, g_accentBrush);
    RECT blueTag{ header.right - 150, accent.top, header.right, header.bottom };
    HBRUSH blue = CreateSolidBrush(kBlueColor);
    FillRect(dc, &blueTag, blue);
    DeleteObject(blue);
}

static INT_PTR PaintStaticColor(HDC dc, HWND control, bool uninstall) {
    const int id = GetDlgCtrlID(control);
    SetBkMode(dc, TRANSPARENT);
    if (IsHeaderControl(id, uninstall)) {
        SetTextColor(dc, (id == IDC_BRAND || id == IDC_UN_BRAND) ? kAccentColor : RGB(250, 252, 255));
        return (INT_PTR)g_headerBrush;
    }
    if (id == IDC_SOURCE_BADGE || id == IDC_UPDATE_BADGE || id == IDC_DEST_BADGE) {
        SetTextColor(dc, kAccentColor);
    } else if (id == IDC_BRAND_TAG || id == IDC_HEADER_SUBTITLE) {
        SetTextColor(dc, kMutedColor);
    } else if (id == IDC_FOOTER) {
        SetTextColor(dc, kMutedColor);
    } else {
        SetTextColor(dc, kTextColor);
    }
    return (INT_PTR)g_bodyBrush;
}

static void DrawBrandButton(const DRAWITEMSTRUCT* draw) {
    const bool isCheck = draw->CtlID == IDC_SC_DESKTOP || draw->CtlID == IDC_SC_STARTMENU || draw->CtlID == IDC_UN_KEEPSAVES;
    if (isCheck) {
        FillRect(draw->hDC, &draw->rcItem, g_bodyBrush);
        RECT box{ draw->rcItem.left, draw->rcItem.top + 1, draw->rcItem.left + 15, draw->rcItem.top + 16 };
        const bool checked = draw->CtlID == IDC_SC_DESKTOP ? g_desktopShortcut :
            draw->CtlID == IDC_SC_STARTMENU ? g_startMenuShortcut : g_removeUserData;
        HBRUSH fill = CreateSolidBrush(checked ? kAccentColor : kFieldColor);
        FillRect(draw->hDC, &box, fill);
        DeleteObject(fill);
        FrameRect(draw->hDC, &box, g_edgeBrush);
        if (checked) {
            HPEN tick = CreatePen(PS_SOLID, 2, kTextColor);
            HGDIOBJ old = SelectObject(draw->hDC, tick);
            MoveToEx(draw->hDC, box.left + 3, box.top + 8, nullptr);
            LineTo(draw->hDC, box.left + 6, box.top + 11);
            LineTo(draw->hDC, box.left + 12, box.top + 4);
            SelectObject(draw->hDC, old);
            DeleteObject(tick);
        }
        SetBkMode(draw->hDC, TRANSPARENT);
        SetTextColor(draw->hDC, (draw->itemState & ODS_DISABLED) ? kMutedColor : kTextColor);
        RECT text = draw->rcItem;
        text.left += 23;
        wchar_t label[128] = {};
        GetWindowTextW(draw->hwndItem, label, (int)(sizeof(label) / sizeof(label[0])));
        DrawTextW(draw->hDC, label, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    const bool primary = draw->CtlID == IDC_INSTALL || draw->CtlID == IDOK;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const COLORREF fill = primary
        ? (pressed ? RGB(170, 32, 44) : kAccentColor)
        : (pressed ? RGB(38, 50, 64) : kFieldColor);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, primary ? kAccentColor : RGB(43, 56, 70));
    HGDIOBJ oldPen = SelectObject(draw->hDC, pen);
    HGDIOBJ oldBrush = SelectObject(draw->hDC, brush);
    RoundRect(draw->hDC, draw->rcItem.left, draw->rcItem.top, draw->rcItem.right, draw->rcItem.bottom,
              primary ? 10 : 7, primary ? 10 : 7);
    SelectObject(draw->hDC, oldBrush); SelectObject(draw->hDC, oldPen); DeleteObject(pen);
    DeleteObject(brush);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, disabled ? kMutedColor : (primary ? kBodyColor : kTextColor));
    wchar_t label[80] = {};
    GetWindowTextW(draw->hwndItem, label, (int)(sizeof(label) / sizeof(label[0])));
    RECT text = draw->rcItem;
    DrawTextW(draw->hDC, label, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (draw->itemState & ODS_FOCUS) {
        InflateRect(&text, -4, -3);
        DrawFocusRect(draw->hDC, &text);
    }
}

static void SetControlsEnabled(HWND dlg, bool on) {
    for (int id : { IDC_ISO_PATH, IDC_ISO_BROWSE, IDC_TU_PATH, IDC_TU_BROWSE, IDC_DEST_PATH, IDC_DEST_BROWSE,
                    IDC_SC_DESKTOP, IDC_SC_STARTMENU, IDC_INSTALL })
        EnableWindow(GetDlgItem(dlg, id), on);
}

static INT_PTR CALLBACK MainProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        EnsureBrandTheme();
        UseDarkTitleBar(dlg);
        HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_TTT2SETUP));
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        ApplyBrandFonts(dlg, false);
        g_desktopShortcut = true;
        g_startMenuShortcut = true;
        std::wstring pf = KnownDir(FOLDERID_ProgramFiles);
        SetDlgItemTextW(dlg, IDC_DEST_PATH, (pf + L"\\" + kFolderName).c_str());
        SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETRANGE32, 0, 100);
        SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETBARCOLOR, 0, (LPARAM)kAccentColor);
        SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETBKCOLOR, 0, (LPARAM)kFieldColor);
        for (int id : { IDC_ISO_PATH, IDC_TU_PATH, IDC_DEST_PATH }) {
            HWND edit = GetDlgItem(dlg, id);
            SetWindowTheme(edit, L"DarkMode_Explorer", nullptr);
            const LONG_PTR style = GetWindowLongPtrW(edit, GWL_STYLE);
            SetWindowLongPtrW(edit, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_BORDER));
            const LONG_PTR exStyle = GetWindowLongPtrW(edit, GWL_EXSTYLE);
            SetWindowLongPtrW(edit, GWL_EXSTYLE, exStyle & ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE));
            SetWindowPos(edit, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            SetWindowSubclass(edit, DarkEditSubclass, (UINT_PTR)id, 0);
        }
        SetWindowTheme(GetDlgItem(dlg, IDC_PROGRESS), L"", L"");
        return TRUE;
    }
    case WM_ERASEBKGND:
        PaintDialogBackground(dlg, (HDC)wp, false);
        return TRUE;
    case WM_CTLCOLORSTATIC:
        return PaintStaticColor((HDC)wp, (HWND)lp, false);
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, kTextColor);
        SetBkColor((HDC)wp, kFieldColor);
        return (INT_PTR)g_fieldBrush;
    case WM_CTLCOLORBTN:
        SetTextColor((HDC)wp, kTextColor);
        SetBkColor((HDC)wp, kBodyColor);
        return (INT_PTR)g_bodyBrush;
    case WM_DRAWITEM:
        DrawBrandButton((const DRAWITEMSTRUCT*)lp);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SC_DESKTOP: g_desktopShortcut = !g_desktopShortcut; InvalidateRect(GetDlgItem(dlg, IDC_SC_DESKTOP), nullptr, TRUE); return TRUE;
        case IDC_SC_STARTMENU: g_startMenuShortcut = !g_startMenuShortcut; InvalidateRect(GetDlgItem(dlg, IDC_SC_STARTMENU), nullptr, TRUE); return TRUE;
        case IDC_ISO_BROWSE: {
            const COMDLG_FILTERSPEC f[] = { { L"Xbox 360 disc image (*.iso)", L"*.iso" }, { L"All files", L"*.*" } };
            std::wstring p = PickFile(dlg, L"Choose your Tekken Tag Tournament 2 disc image", f, 2);
            if (!p.empty()) SetDlgItemTextW(dlg, IDC_ISO_PATH, p.c_str());
            return TRUE;
        }
        case IDC_TU_BROWSE: {
            const COMDLG_FILTERSPEC f[] = { { L"Title update (TU_*)", L"TU_*" }, { L"All files", L"*.*" } };
            std::wstring p = PickFile(dlg, L"Choose Title Update 3 (TU_174Q22P_0000008000000.00000000000O2)", f, 2);
            if (!p.empty()) SetDlgItemTextW(dlg, IDC_TU_PATH, p.c_str());
            return TRUE;
        }
        case IDC_DEST_BROWSE: {
            std::wstring p = PickFolder(dlg);
            if (!p.empty()) SetDlgItemTextW(dlg, IDC_DEST_PATH, (p + L"\\" + kFolderName).c_str());
            return TRUE;
        }
        case IDC_INSTALL: {
            wchar_t iso[MAX_PATH] = {}, tu[MAX_PATH] = {}, dest[MAX_PATH] = {};
            GetDlgItemTextW(dlg, IDC_ISO_PATH, iso, MAX_PATH);
            GetDlgItemTextW(dlg, IDC_TU_PATH, tu, MAX_PATH);
            GetDlgItemTextW(dlg, IDC_DEST_PATH, dest, MAX_PATH);
            if (!iso[0] || GetFileAttributesW(iso) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxW(dlg, L"Choose your disc image first.", kAppName, MB_ICONINFORMATION);
                return TRUE;
            }
            if (!tu[0] || GetFileAttributesW(tu) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxW(dlg, L"Choose Title Update 3 (the file, or the folder holding it).", kAppName, MB_ICONINFORMATION);
                return TRUE;
            }
            if (!dest[0] || wcslen(dest) < 3 || dest[1] != L':') {
            MessageBoxW(dlg, L"Choose a full path to install to, for example C:\\Program Files\\TTT2 Community Edition.",
                            kAppName, MB_ICONINFORMATION);
                return TRUE;
            }
            InitializeCriticalSection(&g_job.lock);
            g_job.dlg = dlg;
            g_job.iso = iso;
            g_job.tu = tu;
            g_job.dest = dest;
            g_job.scDesktop = g_desktopShortcut;
            g_job.scStartMenu = g_startMenuShortcut;
            g_job.cancel = 0;
            SetControlsEnabled(dlg, false);
            g_running = true;
            g_thread = CreateThread(nullptr, 0, InstallThread, &g_job, 0, nullptr);
            return TRUE;
        }
        case IDCANCEL:
            if (g_running) {
                if (MessageBoxW(dlg, L"Stop the installation?", kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES)
                    InterlockedExchange(&g_job.cancel, 1);
                return TRUE;
            }
            EndDialog(dlg, 0);
            return TRUE;
        }
        return FALSE;

    case WM_APP_PROGRESS: {
        EnterCriticalSection(&g_job.lock);
        int pct = g_job.pct;
        std::wstring s = g_job.status;
        LeaveCriticalSection(&g_job.lock);
        SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, pct, 0);
        SetDlgItemTextW(dlg, IDC_STATUS, s.c_str());
        return TRUE;
    }

    case WM_APP_DONE: {
        g_running = false;
        if (g_thread) { WaitForSingleObject(g_thread, 5000); CloseHandle(g_thread); g_thread = nullptr; }
        if (g_job.ok) {
            SetDlgItemTextW(dlg, IDC_STATUS, L"Installed.");
            std::wstring msg = std::wstring(kAppName) +
                L" is installed.\n\nSaved games, profiles and settings are kept in your user profile, so they "
                L"survive an uninstall.\n\nPlay now?";
            if (MessageBoxW(dlg, msg.c_str(), kAppName, MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                std::wstring launcher = g_job.dest + L"\\" + kLauncherExeName;
                ShellExecuteW(nullptr, L"open", launcher.c_str(), nullptr, g_job.dest.c_str(), SW_SHOWNORMAL);
            }
            EndDialog(dlg, 1);
        } else {
            SetDlgItemTextW(dlg, IDC_STATUS, L"");
            SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);
            MessageBoxW(dlg, g_job.error.empty() ? L"The installation did not finish." : g_job.error.c_str(),
                        kAppName, MB_ICONERROR);
            SetControlsEnabled(dlg, true);
        }
        return TRUE;
    }

    case WM_CLOSE:
        if (!g_running) EndDialog(dlg, 0);
        return TRUE;
    case WM_DESTROY:
        for (HFONT font : { g_brandFont, g_titleFont, g_sectionFont, g_captionFont })
            if (font) { DeleteObject(font); }
        g_brandFont = g_titleFont = g_sectionFont = g_captionFont = nullptr;
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------- uninstall

struct UninstallState { std::wstring dir; bool alsoSaves = false; };
static UninstallState g_un;

// Uninstalling deletes the folder it runs from, so it logs to %TEMP%.
static void UnLog(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, path)) return;
    wcscat_s(path, L"ttt2_uninstall.log");
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    wchar_t line[1024];
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf_s(line, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 0) {
        std::wstring w = std::wstring(line) + L"\r\n";
        int b = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string utf8((size_t)(b > 0 ? b : 0), '\0');
        if (b > 0) {
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &utf8[0], b, nullptr, nullptr);
            DWORD put = 0;
            WriteFile(h, utf8.data(), (DWORD)utf8.size(), &put, nullptr);
        }
    }
    CloseHandle(h);
}

static void DoUninstall(HWND dlg) {
    UnLog(L"--- uninstall: dir=\"%s\" alsoSaves=%d", g_un.dir.c_str(), g_un.alsoSaves ? 1 : 0);
    for (REFKNOWNFOLDERID id : { FOLDERID_Desktop, FOLDERID_CommonPrograms, FOLDERID_Programs }) {
        std::wstring d = KnownDir(id);
        if (!d.empty()) DeleteFileW((d + L"\\" + kShortcut).c_str());
    }
    if (dlg) SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 40, 0);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kRegKey);
    if (dlg) SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 55, 0);
    if (g_un.alsoSaves) {
        std::wstring local = KnownDir(FOLDERID_LocalAppData);
        if (!local.empty()) DeleteTree(local + L"\\" + kUserDataDir);
    }
    if (dlg) SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 70, 0);
    // Re-check immediately before the destructive call: never delete a tree that is not ours.
    if (!g_un.dir.empty() && LooksLikeInstall(g_un.dir)) {
        DeleteTree(g_un.dir);
        UnLog(L"    tree deleted; dir still present = %d", GetFileAttributesW(g_un.dir.c_str()) != INVALID_FILE_ATTRIBUTES ? 1 : 0);
    } else {
        UnLog(L"    REFUSED to delete \"%s\" -- not an install folder", g_un.dir.c_str());
    }
    if (dlg) SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 100, 0);

    // Our own exe is still locked: a detached cmd removes it once we exit.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"/c ping 127.0.0.1 -n 3 >nul & del /f /q \"" + std::wstring(self) + L"\" & rmdir \"" + g_un.dir + L"\"";
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring shell = std::wstring(sys) + L"\\cmd.exe";
    std::wstring line = L"\"" + shell + L"\" " + cmd;
    std::vector<wchar_t> buf(line.begin(), line.end());
    buf.push_back(0);
    if (CreateProcessW(shell.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}

static INT_PTR CALLBACK UninstallProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        EnsureBrandTheme();
        UseDarkTitleBar(dlg);
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_TTT2SETUP)));
        ApplyBrandFonts(dlg, true);
        std::wstring t = std::wstring(kAppName) + L" will be removed from:\n" + g_un.dir +
                         L"\n\nYour saved games and profiles are kept unless you tick the box below.";
        SetDlgItemTextW(dlg, IDC_UN_TEXT, t.c_str());
        g_removeUserData = false;
        SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETRANGE32, 0, 100);
        SetWindowTheme(GetDlgItem(dlg, IDC_PROGRESS), L"", L"");
        return TRUE;
    }
    case WM_ERASEBKGND:
        PaintDialogBackground(dlg, (HDC)wp, true);
        return TRUE;
    case WM_CTLCOLORSTATIC:
        return PaintStaticColor((HDC)wp, (HWND)lp, true);
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, kTextColor);
        SetBkColor((HDC)wp, kFieldColor);
        return (INT_PTR)g_fieldBrush;
    case WM_CTLCOLORBTN:
        SetTextColor((HDC)wp, kTextColor);
        SetBkColor((HDC)wp, kBodyColor);
        return (INT_PTR)g_bodyBrush;
    case WM_DRAWITEM:
        DrawBrandButton((const DRAWITEMSTRUCT*)lp);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_UN_KEEPSAVES) { g_removeUserData = !g_removeUserData; InvalidateRect(GetDlgItem(dlg, IDC_UN_KEEPSAVES), nullptr, TRUE); return TRUE; }
        if (LOWORD(wp) == IDOK) {
            g_un.alsoSaves = g_removeUserData;
            if (g_un.alsoSaves &&
                MessageBoxW(dlg, L"Your saved games, profiles and settings will be permanently deleted. Continue?",
                            kAppName, MB_YESNO | MB_ICONWARNING) != IDYES)
                return TRUE;
            EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
            EnableWindow(GetDlgItem(dlg, IDCANCEL), FALSE);
            DoUninstall(dlg);
            EndDialog(dlg, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        return FALSE;
    case WM_CLOSE:
        EndDialog(dlg, 0);
        return TRUE;
    case WM_DESTROY:
        for (HFONT font : { g_brandFont, g_titleFont, g_sectionFont, g_captionFont })
            if (font) DeleteObject(font);
        g_brandFont = g_titleFont = g_sectionFont = g_captionFont = nullptr;
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------- entry

static bool HasArg(const wchar_t* cmdline, const wchar_t* flag) {
    std::wstring c(cmdline);
    for (auto& ch : c) ch = (wchar_t)towlower(ch);
    return c.find(flag) != std::wstring::npos;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int) {
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();

    // The installer copies itself in as uninstall.exe: the file name decides which program we
    // are (a double-click passes no arguments), /uninstall too.
    wchar_t selfPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    const wchar_t* selfName = wcsrchr(selfPath, L'\\');
    selfName = selfName ? selfName + 1 : selfPath;
    const bool amUninstaller = _wcsicmp(selfName, kUninstallExe) == 0 || HasArg(cmdline, L"/uninstall");

    int rc = 0;
    // /silent <disc.iso> <title update> <install folder>: the same install with no UI and a log
    // next to the exe (scripted tests; unattended installs).
    if (HasArg(cmdline, L"/silent")) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring iso, tu, dest;
        for (int i = 1; i < argc; ++i)
            if (_wcsicmp(argv[i], L"/silent") == 0) {
                if (i + 3 < argc) { iso = argv[i + 1]; tu = argv[i + 2]; dest = argv[i + 3]; }
                break;
            }
        if (argv) LocalFree(argv);
        if (iso.empty() || tu.empty() || dest.empty()) {
            MessageBoxW(nullptr, L"usage: ttt2_setup.exe /silent <disc.iso> <title update file or folder> <install folder>",
                        kAppName, MB_ICONINFORMATION);
            CoUninitialize();
            return 2;
        }
        const std::wstring logPath = std::wstring(selfPath) + L".log";
        InitializeCriticalSection(&g_job.lock);
        g_job.dlg = nullptr;
        g_job.iso = iso;
        g_job.tu = tu;
        g_job.dest = dest;
        g_job.scDesktop = g_job.scStartMenu = false;
        g_job.log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_job.log != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            WriteFile(g_job.log, "\xEF\xBB\xBF", 3, &put, nullptr);
        }
        g_job.Set(0, L"[args] <" + std::wstring(cmdline ? cmdline : L"") + L">");
        const DWORD start = GetTickCount();
        const bool ok = Install(&g_job);
        wchar_t took[64];
        swprintf_s(took, L"[time] %.1f s", (GetTickCount() - start) / 1000.0);
        g_job.Set(g_job.pct, took);
        if (g_job.log != INVALID_HANDLE_VALUE) CloseHandle(g_job.log);
        CoUninitialize();
        return ok ? 0 : 1;
    }
    if (amUninstaller) {
        std::wstring dir = selfPath;
        size_t at = dir.find_last_of(L'\\');
        if (at != std::wstring::npos) dir.resize(at);
        if (!LooksLikeInstall(dir)) {
            std::wstring fromReg;
            HKEY k = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
                wchar_t v[MAX_PATH] = {}; DWORD n = sizeof v, type = 0;
                if (RegQueryValueExW(k, L"InstallLocation", nullptr, &type, (BYTE*)v, &n) == ERROR_SUCCESS) fromReg = v;
                RegCloseKey(k);
            }
            dir = LooksLikeInstall(fromReg) ? fromReg : std::wstring();
        }
        if (dir.empty()) {
            UnLog(L"--- refused: \"%s\" is not an install folder and the registry did not name a valid one", selfPath);
            MessageBoxW(nullptr,
                        L"This uninstaller could not find the installed game.\n\nNothing has been changed. Run the "
                        L"uninstaller from inside the game's own folder, or use Settings > Apps.",
                        kAppName, MB_ICONWARNING);
            CoUninitialize();
            return 1;
        }
        g_un.dir = dir;
        if (HasArg(cmdline, L"/quiet")) { DoUninstall(nullptr); rc = 0; }
        else rc = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_UNINSTALL), nullptr, UninstallProc, 0) == 1 ? 0 : 1;
    } else {
        rc = (int)DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, MainProc, 0);
    }
    CoUninitialize();
    return rc;
}
