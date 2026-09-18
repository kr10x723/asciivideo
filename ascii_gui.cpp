#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#define WINVER 0x0600
#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <commdlg.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cwchar>

static const wchar_t CLASS_NAME[] = L"AsciiPlayerWnd";
static const wchar_t RAMP[] = L"01";

enum { IDM_OPEN = 1, IDM_SAVECOPY, IDM_PAUSE, IDM_ASPECT, IDM_ZOOMIN, IDM_ZOOMOUT, IDM_EXIT, IDM_INFO,
       IDM_OPENWITH, IDM_REMOVEOPENWITH,
       IDM_RECENT = 20 };

static const int FONT_SIZES[] = { 10, 12, 14, 18, 22, 28, 36 };
static const int FONT_SIZES_N = 7;
static const int STATUS_H = 34;

struct VideoInfo {
    int width = 0;
    int height = 0;
    double fps = 24.0;
    double duration = 0.0;
};

static std::wstring findInWinget(const wchar_t* name) {
    wchar_t la[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, la) != S_OK) return L"";
    std::wstring base = std::wstring(la) + L"\\Microsoft\\WinGet\\Packages\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring sub = base + fd.cFileName + L"\\";
        WIN32_FIND_DATAW fd2;
        HANDLE h2 = FindFirstFileW((sub + L"*").c_str(), &fd2);
        if (h2 == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring cand = sub + fd2.cFileName + L"\\bin\\" + name;
            if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
                found = cand;
                FindClose(h2);
                FindClose(h);
                return found;
            }
        } while (FindNextFileW(h2, &fd2));
        FindClose(h2);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static std::wstring findExe(const wchar_t* name) {
    wchar_t* env = _wgetenv(L"PATH");
    if (env) {
        wchar_t* tmp = _wcsdup(env);
        wchar_t* context = NULL;
        wchar_t* dir = wcstok_s(tmp, L";", &context);
        while (dir) {
            std::wstring cand = std::wstring(dir) + L"\\" + name;
            if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
                free(tmp);
                return cand;
            }
            dir = wcstok_s(NULL, L";", &context);
        }
        free(tmp);
    }
    std::wstring winget = findInWinget(name);
    if (!winget.empty()) return winget;
    const wchar_t* fallbacks[] = {
        L"C:\\ffmpeg\\bin",
        L"C:\\w64devkit",
    };
    for (const wchar_t* d : fallbacks) {
        std::wstring cand = std::wstring(d) + L"\\" + name;
        if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) return cand;
    }
    return L"";
}

static std::wstring winCmd(const std::vector<std::wstring>& args) {
    std::wstring cmd;
    for (const std::wstring& a : args) {
        if (!cmd.empty()) cmd += L" ";
        cmd += L"\"" + a + L"\"";
    }
    return cmd;
}

static std::wstring runCaptureW(const std::wstring& exe, const std::vector<std::wstring>& args) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hRead = NULL, hWrite = NULL;
    HANDLE eRead = NULL, eWrite = NULL;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    CreatePipe(&eRead, &eWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(eRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = eWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring cmdLine = L"\"" + exe + L"\" " + winCmd(args);
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');
    if (!CreateProcessW(exe.c_str(), cmdBuf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hRead); CloseHandle(hWrite);
        CloseHandle(eRead); CloseHandle(eWrite);
        return L"";
    }
    CloseHandle(hWrite);
    CloseHandle(eWrite);
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(eRead, buf, sizeof(buf), &n, NULL) && n > 0) {}
    CloseHandle(eRead);
    std::wstring out;
    while (ReadFile(hRead, buf, sizeof(buf), &n, NULL) && n > 0) {
        for (DWORD i = 0; i < n; ++i) out += (wchar_t)buf[i];
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return out;
}

static VideoInfo probeVideo(const std::wstring& ffprobe, const std::wstring& file) {
    VideoInfo vi;
    std::wstring out = runCaptureW(ffprobe, {
        L"-v", L"error", L"-select_streams", L"v:0",
        L"-show_entries", L"stream=width,height,avg_frame_rate,r_frame_rate:format=duration",
        L"-of", L"default=noprint_wrappers=1", file
    });
    size_t pos = 0;
    while (pos < out.size()) {
        size_t end = out.find(L'\n', pos);
        std::wstring line = out.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
        pos = (end == std::wstring::npos) ? out.size() : end + 1;
        auto grab = [&](const wchar_t* key) -> std::wstring {
            size_t p = line.find(key);
            if (p == std::wstring::npos) return L"";
            return line.substr(p + wcslen(key));
        };
        std::wstring w = grab(L"width=");
        std::wstring h = grab(L"height=");
        if (!w.empty()) vi.width = _wtoi(w.c_str());
        if (!h.empty()) vi.height = _wtoi(h.c_str());
        std::wstring v = grab(L"avg_frame_rate=");
        if (v.empty()) v = grab(L"r_frame_rate=");
        if (!v.empty()) {
            size_t s = v.find(L'/');
            double num = _wtof(v.substr(0, s).c_str());
            double den = (s == std::wstring::npos) ? 1.0 : _wtof(v.substr(s + 1).c_str());
            if (den > 0 && num > 0) vi.fps = num / den;
        }
        std::wstring d = grab(L"duration=");
        if (!d.empty()) vi.duration = _wtof(d.c_str());
    }
    if (vi.fps <= 0 || vi.fps > 240) vi.fps = 24.0;
    return vi;
}

static HANDLE g_pipeRead = NULL;
static HANDLE g_ffmpegProc = NULL;
static volatile LONG g_running = 0;
static volatile LONG g_paused = 0;
static volatile LONG g_showInfo = 1;
static volatile LONG g_aspect = 0;
static volatile int  g_fontIndex = 3;
static VideoInfo g_vi;

static HANDLE g_resumeEvent = NULL;
static HANDLE g_workerThread = NULL;
static HANDLE g_audioRead = NULL;
static HANDLE g_audioProc = NULL;
static HANDLE g_audioThread = NULL;
static HWAVEOUT g_waveOut = NULL;
static HWND g_hwnd = NULL;
static std::wstring g_fileName;
static std::wstring g_ffprobePath;
static std::wstring g_ffmpegPath;
static std::vector<std::wstring> g_recent;
static std::wstring g_lastDir;
static HMENU g_fileMenu = NULL;
static volatile LONG g_exporting = 0;
static HANDLE g_exportThread = NULL;
static std::wstring g_exportPath;
static void loadSettings() {
    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AsciiPlayer", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD sz = 0;
        if (RegQueryValueExW(k, L"LastDir", NULL, NULL, NULL, &sz) == ERROR_SUCCESS && sz >= 2) {
            std::vector<wchar_t> b(sz / 2);
            DWORD t = sz;
            if (RegQueryValueExW(k, L"LastDir", NULL, NULL, (LPBYTE)b.data(), &t) == ERROR_SUCCESS)
                g_lastDir = b.data();
        }
        sz = 0;
        if (RegQueryValueExW(k, L"Recent", NULL, NULL, NULL, &sz) == ERROR_SUCCESS && sz > 2) {
            std::vector<wchar_t> b(sz / 2 + 1);
            DWORD t = sz;
            if (RegQueryValueExW(k, L"Recent", NULL, NULL, (LPBYTE)b.data(), &t) == ERROR_SUCCESS) {
                const wchar_t* p = b.data();
                while (*p && g_recent.size() < 16) {
                    g_recent.push_back(p);
                    p += wcslen(p) + 1;
                }
            }
        }
        RegCloseKey(k);
    }
}

static void saveSettings() {
    HKEY k = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AsciiPlayer", 0, NULL, 0,
            KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS || !k)
        return;
    if (!g_lastDir.empty())
        RegSetValueExW(k, L"LastDir", 0, REG_SZ, (const BYTE*)g_lastDir.c_str(),
            (DWORD)((g_lastDir.size() + 1) * 2));
    std::vector<wchar_t> mz;
    for (const auto& s : g_recent) {
        mz.insert(mz.end(), s.begin(), s.end());
        mz.push_back(L'\0');
    }
    mz.push_back(L'\0');
    if (!mz.empty())
        RegSetValueExW(k, L"Recent", 0, REG_MULTI_SZ, (const BYTE*)mz.data(),
            (DWORD)(mz.size() * 2));
    RegCloseKey(k);
}

static LONG WINAPI crashFilter(PEXCEPTION_POINTERS ep) {
    FILE* f = _wfopen(L"ascii_crash.txt", L"a");
    if (f) {
        fprintf(f, "ascii_gui exception code=0x%08lX address=%p thread=%lu\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress,
            (unsigned long)GetCurrentThreadId());
        fclose(f);
    }
    MessageBoxW(NULL, L"ASCII Player crashed.\nDetails written to ascii_crash.txt",
        L"ASCII Player", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool regSet(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* val) {
    HKEY k;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val,
        (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static void installOpenWith() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + exe + L"\" \"%1\"";
    std::wstring icon = std::wstring(L"\"") + exe + L"\",0";

    std::wstring app = L"Software\\Classes\\Applications\\ascii_gui.exe";
    regSet(HKEY_CURRENT_USER, app.c_str(), L"FriendlyAppName", L"ASCII Player");
    regSet(HKEY_CURRENT_USER, app.c_str(), L"DefaultIcon", icon.c_str());
    regSet(HKEY_CURRENT_USER, (app + L"\\shell\\open\\command").c_str(), NULL, cmd.c_str());
    regSet(HKEY_CURRENT_USER, (app + L"\\SupportedTypes").c_str(), L".mp4", L"");

    std::wstring prog = L"Software\\Classes\\AsciiPlayer.mp4";
    regSet(HKEY_CURRENT_USER, prog.c_str(), NULL, L"ASCII Video");
    regSet(HKEY_CURRENT_USER, (prog + L"\\DefaultIcon").c_str(), NULL, icon.c_str());
    regSet(HKEY_CURRENT_USER, (prog + L"\\shell\\open\\command").c_str(), NULL, cmd.c_str());
    regSet(HKEY_CURRENT_USER, L"Software\\Classes\\.mp4\\OpenWithProgids", L"AsciiPlayer.mp4", L"");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

static void uninstallOpenWith() {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\ascii_gui.exe");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\AsciiPlayer.mp4");
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.mp4\\OpenWithProgids",
            0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, L"AsciiPlayer.mp4");
        RegCloseKey(k);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

static void rebuildFileMenu(HMENU bar) {
    if (g_fileMenu) {
        RemoveMenu(bar, 0, MF_BYPOSITION);
        DestroyMenu(g_fileMenu);
        g_fileMenu = NULL;
    }
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(m, MF_STRING, IDM_SAVECOPY, L"Save ASCII &Video...\tCtrl+S");
    if (!g_recent.empty()) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        for (int i = 0; i < (int)g_recent.size() && i < 8; ++i) {
            std::wstring label = L"&" + std::to_wstring(i + 1) + L"  ";
            size_t pos = g_recent[i].find_last_of(L"\\/");
            label += (pos == std::wstring::npos) ? g_recent[i] : g_recent[i].substr(pos + 1);
            AppendMenuW(m, MF_STRING, IDM_RECENT + i, label.c_str());
        }
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit");
    g_fileMenu = m;
    InsertMenuW(bar, 0, MF_BYPOSITION | MF_POPUP, (UINT_PTR)m, L"&File");
}

static bool readExact(HANDLE h, unsigned char* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        DWORD n = 0;
        if (!ReadFile(h, buf + got, (DWORD)(len - got), &n, NULL) || n == 0) return false;
        got += n;
    }
    return true;
}

static HANDLE spawnProc(const std::wstring& exe, const std::vector<std::wstring>& args,
                        HANDLE* hRead, bool stderrToNull) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hChildRead = NULL, hChildWrite = NULL;
    if (!CreatePipe(&hChildRead, &hChildWrite, &sa, 0)) return NULL;
    SetHandleInformation(hChildRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE hErr = NULL;
    if (stderrToNull)
        hErr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    else
        hErr = GetStdHandle(STD_ERROR_HANDLE);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hChildWrite;
    si.hStdError = hErr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring cmdLine = L"\"" + exe + L"\" " + winCmd(args);
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');
    if (!CreateProcessW(exe.c_str(), cmdBuf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hChildRead);
        CloseHandle(hChildWrite);
        if (stderrToNull && hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
        return NULL;
    }
    CloseHandle(hChildWrite);
    if (stderrToNull && hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
    CloseHandle(pi.hThread);
    *hRead = hChildRead;
    return pi.hProcess;
}

static HANDLE spawnFfmpeg(const std::wstring& ffmpeg, const std::wstring& file, HANDLE* hRead, double startSec) {
    std::vector<std::wstring> args;
    if (startSec > 0) {
        wchar_t ss[32];
        swprintf(ss, 32, L"%.3f", startSec);
        args.push_back(L"-ss");
        args.push_back(ss);
    }
    args.push_back(L"-v"); args.push_back(L"error");
    args.push_back(L"-i"); args.push_back(file);
    args.push_back(L"-an");
    args.push_back(L"-f"); args.push_back(L"rawvideo");
    args.push_back(L"-pix_fmt"); args.push_back(L"rgb24");
    args.push_back(L"pipe:1");
    return spawnProc(ffmpeg, args, hRead, true);
}

static HANDLE spawnFfmpegAudio(const std::wstring& ffmpeg, const std::wstring& file, HANDLE* hRead, double startSec) {
    std::vector<std::wstring> args;
    args.push_back(L"-v"); args.push_back(L"quiet");
    if (startSec > 0) {
        wchar_t ss[32];
        swprintf(ss, 32, L"%.3f", startSec);
        args.push_back(L"-ss");
        args.push_back(ss);
    }
    args.push_back(L"-i"); args.push_back(file);
    args.push_back(L"-map"); args.push_back(L"0:a:0?");
    args.push_back(L"-ac"); args.push_back(L"2");
    args.push_back(L"-ar"); args.push_back(L"44100");
    args.push_back(L"-f"); args.push_back(L"s16le");
    args.push_back(L"pipe:1");
    return spawnProc(ffmpeg, args, hRead, true);
}

static HANDLE spawnStdinProc(const std::wstring& exe, const std::vector<std::wstring>& args, HANDLE* pWrite) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rRead = NULL, rWrite = NULL;
    if (!CreatePipe(&rRead, &rWrite, &sa, 0)) return NULL;
    SetHandleInformation(rWrite, HANDLE_FLAG_INHERIT, 0);
    HANDLE hErr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = rRead;
    si.hStdOutput = hErr;
    si.hStdError = hErr;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring cmdLine = L"\"" + exe + L"\" " + winCmd(args);
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');
    if (!CreateProcessW(exe.c_str(), cmdBuf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rRead);
        CloseHandle(rWrite);
        if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
        return NULL;
    }
    CloseHandle(rRead);
    if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
    CloseHandle(pi.hThread);
    *pWrite = rWrite;
    return pi.hProcess;
}

static volatile long framesPlayed = 0;
static double g_seekBase = 0.0;

#define WM_APP_FRAME (WM_APP + 1)
#define WM_APP_EXPORT_DONE (WM_APP + 2)

static CRITICAL_SECTION g_frameCs;
static std::vector<unsigned char> g_simR, g_simG, g_simB;
static std::vector<wchar_t> g_simCh;
static int g_simCols = 0, g_simRows = 0;
static LONG g_hasFrame = 0;
static std::vector<unsigned char> g_raw;
static int g_rawW = 0, g_rawH = 0;

static const int AUDIO_BUFS = 8;
static const int AUDIO_CHUNK = 16384;
static const int AUDIO_SR = 44100;

struct GridInfo {
    int cellW, cellH, cols, rows, xOff, yOff;
};

static GridInfo computeGrid(int cw, int ch) {
    GridInfo gi = {};
    if (ch < 10) ch = 10;
    HDC hdc = GetDC(g_hwnd);
    HFONT font = CreateFontW((int)(FONT_SIZES[g_fontIndex] * 1.4), 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ oldFont = SelectObject(hdc, font);
    TEXTMETRICW tm = {};
    GetTextMetricsW(hdc, &tm);
    int cellW = tm.tmAveCharWidth;
    int cellH = tm.tmHeight;
    SelectObject(hdc, oldFont);
    DeleteObject(font);
    ReleaseDC(g_hwnd, hdc);
    if (cellW < 1) cellW = 1;
    if (cellH < 1) cellH = 1;
    int cols = cw / cellW;
    int rows = ch / cellH;
    if (cols < 4) cols = 4;
    if (rows < 2) rows = 2;
    if (g_aspect && g_vi.width > 0 && g_vi.height > 0) {
        int arRows = (int)((long long)cols * g_vi.height * 2 / (long long)g_vi.width);
        if (arRows < rows) rows = arRows;
    }
    gi.cellW = cellW;
    gi.cellH = cellH;
    gi.cols = cols;
    gi.rows = rows;
    gi.xOff = (cw - cols * cellW) / 2;
    gi.yOff = (ch - rows * cellH) / 2;
    return gi;
}

static void buildAscii(const unsigned char* frame, int W, int H, int cols, int rows,
                       std::vector<unsigned char>& r, std::vector<unsigned char>& g,
                       std::vector<unsigned char>& b, std::vector<wchar_t>& ch) {
    int cells = cols * rows;
    r.assign(cells, 0); g.assign(cells, 0); b.assign(cells, 0); ch.assign(cells, L'0');
    std::vector<int> lum(cells);
    int hist[256] = {};
    for (int yy = 0; yy < rows; ++yy) {
        int y0 = (int)((long long)yy * H / rows);
        int y1 = (int)((long long)(yy + 1) * H / rows);
        if (y1 <= y0) y1 = y0 + 1;
        for (int cc = 0; cc < cols; ++cc) {
            int x0 = (int)((long long)cc * W / cols);
            int x1 = (int)((long long)(cc + 1) * W / cols);
            if (x1 <= x0) x1 = x0 + 1;
            long sr = 0, sg = 0, sb = 0, n = 0;
            for (int y = y0; y < y1; ++y) {
                const unsigned char* p = &frame[y * W * 3 + x0 * 3];
                for (int x = x0; x < x1; ++x) {
                    sr += p[0]; sg += p[1]; sb += p[2];
                    p += 3; ++n;
                }
            }
            int i = yy * cols + cc;
            r[i] = (unsigned char)(sr / n);
            g[i] = (unsigned char)(sg / n);
            b[i] = (unsigned char)(sb / n);
            lum[i] = (int)((299 * (unsigned long)r[i] + 587 * (unsigned long)g[i] + 114 * (unsigned long)b[i]) / 1000);
            ++hist[lum[i]];
        }
    }
    int lo = 0, hi = 255;
    if (cells > 0) {
        long run = 0;
        for (int i = 0; i < 256; ++i) { run += hist[i]; if (run >= cells / 50) { lo = i; break; } }
        run = 0;
        for (int i = 255; i >= 0; --i) { run += hist[i]; if (run >= cells / 50) { hi = i; break; } }
        if (hi <= lo) { lo = 0; hi = 255; }
    }
    int span = hi - lo;
    int rampN = (int)wcslen(RAMP);
    for (int i = 0; i < cells; ++i) {
        int mlum = lum[i];
        if (span > 0) {
            mlum = (lum[i] - lo) * 255 / span;
            if (mlum < 0) mlum = 0;
            if (mlum > 255) mlum = 255;
        }
        int idx = (mlum * (rampN - 1)) / 255;
        if (idx < 0) idx = 0;
        if (idx > rampN - 1) idx = rampN - 1;
        ch[i] = RAMP[idx];
    }
}

static void decodeAndStore(const unsigned char* frame) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right - rc.left;
    int chH = rc.bottom - rc.top - STATUS_H;
    GridInfo gi = computeGrid(cw, chH);
    std::vector<unsigned char> r, g, b;
    std::vector<wchar_t> ch;
    buildAscii(frame, g_vi.width, g_vi.height, gi.cols, gi.rows, r, g, b, ch);
    EnterCriticalSection(&g_frameCs);
    g_raw.assign(frame, frame + (size_t)g_vi.width * g_vi.height * 3);
    g_rawW = g_vi.width;
    g_rawH = g_vi.height;
    g_simR.swap(r);
    g_simG.swap(g);
    g_simB.swap(b);
    g_simCh.swap(ch);
    g_simCols = gi.cols;
    g_simRows = gi.rows;
    LeaveCriticalSection(&g_frameCs);
    InterlockedExchange(&g_hasFrame, 1);
    PostMessageW(g_hwnd, WM_APP_FRAME, 0, 0);
}

static void fmtTime(double sec, wchar_t* out) {
    if (sec < 0) sec = 0;
    int m = (int)(sec / 60);
    int s = (int)sec % 60;
    swprintf(out, 16, L"%02d:%02d", m, s);
}

static HDC g_bufDc = NULL;
static HBITMAP g_bufBmp = NULL;
static int g_bufW = 0, g_bufH = 0;

static void ensureBackBuffer(HDC hdc, int cw, int ch) {
    if (!g_bufDc) g_bufDc = CreateCompatibleDC(hdc);
    if (!g_bufBmp || cw > g_bufW || ch > g_bufH) {
        if (g_bufBmp) DeleteObject(g_bufBmp);
        g_bufBmp = CreateCompatibleBitmap(hdc, cw, ch);
        g_bufW = cw; g_bufH = ch;
        SelectObject(g_bufDc, g_bufBmp);
    }
}

static void drawFrame(HDC hdc) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    int vidH = ch - STATUS_H;
    if (vidH < 10) vidH = 10;
    ensureBackBuffer(hdc, cw, ch);
    HDC dc = g_bufDc;

    bool has = g_hasFrame != 0;
    GridInfo gi = computeGrid(cw, vidH);
    int cols = gi.cols, rows = gi.rows, xOff = gi.xOff, yOff = gi.yOff;
    std::vector<unsigned char> r, g, b;
    std::vector<wchar_t> chs;
    if (has) {
        EnterCriticalSection(&g_frameCs);
        if (g_simCols == gi.cols && g_simRows == gi.rows && !g_simCh.empty()) {
            r = g_simR; g = g_simG; b = g_simB; chs = g_simCh;
        } else if (!g_raw.empty() && g_rawW == g_vi.width && g_rawH == g_vi.height) {
            buildAscii(g_raw.data(), g_vi.width, g_vi.height, gi.cols, gi.rows, r, g, b, chs);
        }
        LeaveCriticalSection(&g_frameCs);
        if (chs.empty()) has = false;
    }
    RECT full = { 0, 0, cw, ch };
    FillRect(dc, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));

    if (!has) {
        SetTextColor(dc, RGB(120, 120, 120));
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, GetStockObject(SYSTEM_FONT));
        RECT pla = { 0, 0, cw, vidH };
        DrawTextW(dc, L"Drop a video file here -or press [Ctrl+O]",
            -1, &pla, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        HFONT font = CreateFontW((int)(FONT_SIZES[g_fontIndex] * 1.4), 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
        HGDIOBJ f = SelectObject(dc, font);
        TEXTMETRICW tm = {};
        GetTextMetricsW(dc, &tm);
        SetBkMode(dc, TRANSPARENT);
        SetTextCharacterExtra(dc, gi.cellW - tm.tmAveCharWidth);
        int prevR = -2, prevG = -2, prevB = -2;
        int n = cols * rows;
        for (int i = 0; i < n; ++i) {
            int rr = r[i], gg = g[i], bb = b[i];
            if (rr != prevR || gg != prevG || bb != prevB) {
                SetTextColor(dc, RGB(rr, gg, bb));
                prevR = rr; prevG = gg; prevB = bb;
            }
            int c = i % cols;
            int yy = i / cols;
            wchar_t one[2] = { chs[i], 0 };
            TextOutW(dc, xOff + c * gi.cellW, yOff + yy * gi.cellH, one, 1);
        }
        SelectObject(dc, f);
        DeleteObject(font);
    }

    HBRUSH sep = CreateSolidBrush(RGB(45, 45, 45));
    RECT sepR = { 0, ch - STATUS_H, cw, ch - STATUS_H + 1 };
    FillRect(dc, &sepR, sep);
    DeleteObject(sep);

    if (g_showInfo) {
        wchar_t cur[16], tot[16];
        double tpos = g_seekBase + (g_vi.fps > 0 ? (double)framesPlayed / g_vi.fps : 0.0);
        fmtTime(tpos, cur);
        bool hasDur = g_vi.duration > 0;
        fmtTime(hasDur ? g_vi.duration : 0.0, tot);
        wchar_t info[384];
        if (has) {
            swprintf(info, 384,
                L"%ls   |   %dx%d video   %dx%d ascii   %.0f fps   %ls / %ls",
                g_fileName.c_str(), g_vi.width, g_vi.height, cols, rows,
                g_vi.fps, cur, hasDur ? tot : L"--:--");
        } else {
            swprintf(info, 384, L"%ls", g_fileName.c_str());
        }
        SetTextColor(dc, RGB(140, 140, 140));
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, GetStockObject(SYSTEM_FONT));
        RECT tr = { 8, ch - STATUS_H + 4, cw - 8, ch - 14 };
        DrawTextW(dc, info, -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT bar = { 8, ch - 11, cw - 8, ch - 7 };
        HBRUSH track = CreateSolidBrush(RGB(28, 28, 28));
        FillRect(dc, &bar, track);
        DeleteObject(track);
        if (has && hasDur && g_vi.duration > 0) {
            double frac = tpos / g_vi.duration;
            if (frac > 1.0) frac = 1.0;
            int fw = (int)((bar.right - bar.left) * frac);
            if (fw > 0) {
                RECT fi = { bar.left, bar.top, bar.left + fw, bar.bottom };
                HBRUSH fillb = CreateSolidBrush(RGB(64, 170, 96));
                FillRect(dc, &fi, fillb);
                DeleteObject(fillb);
            }
        }
    }
    BitBlt(hdc, 0, 0, cw, ch, dc, 0, 0, SRCCOPY);
}

static DWORD WINAPI renderThread(LPVOID) {
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        if (g_paused) {
            WaitForSingleObject(g_resumeEvent, 500);
            continue;
        }
        size_t frameSize = (size_t)g_vi.width * g_vi.height * 3;
        std::vector<unsigned char> frame(frameSize);
        if (!readExact(g_pipeRead, frame.data(), frameSize)) {
            break;
        }
        long fn = ++framesPlayed;

        static std::chrono::steady_clock::time_point start;
        if (fn == 1) start = std::chrono::steady_clock::now();
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        long long targetMs = (long long)(fn * 1000.0 / (g_vi.fps > 0 ? g_vi.fps : 24.0));
        long long sleepMs = targetMs - nowMs;

        decodeAndStore(frame.data());
        if (sleepMs > 0) Sleep((DWORD)sleepMs);
    }
    if (g_audioProc) {
        TerminateProcess(g_audioProc, 0);
        CloseHandle(g_audioProc);
        g_audioProc = NULL;
    }
    InterlockedExchange(&g_running, 0);
    return 0;
}

static DWORD WINAPI audioThread(LPVOID) {
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = AUDIO_SR;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = AUDIO_SR * 4;
    HWAVEOUT wo = NULL;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return 0;
    g_waveOut = wo;
    std::vector<std::vector<char>> bufs(AUDIO_BUFS, std::vector<char>(AUDIO_CHUNK));
    std::vector<WAVEHDR> hdrs(AUDIO_BUFS);
    std::vector<bool> inUse(AUDIO_BUFS, false);
    for (int i = 0; i < AUDIO_BUFS; ++i) {
        ZeroMemory(&hdrs[i], sizeof(hdrs[i]));
        hdrs[i].lpData = bufs[i].data();
        hdrs[i].dwBufferLength = AUDIO_CHUNK;
        hdrs[i].dwUser = (DWORD)i;
    }
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        if (g_paused) {
            WaitForSingleObject(g_resumeEvent, 200);
            continue;
        }
        int w = -1;
        for (int i = 0; i < AUDIO_BUFS; ++i) {
            if (!inUse[i]) { w = i; break; }
            if (hdrs[i].dwFlags & WHDR_DONE) {
                waveOutUnprepareHeader(wo, &hdrs[i], sizeof(hdrs[i]));
                inUse[i] = false;
                w = i;
                break;
            }
        }
        if (w < 0) { Sleep(5); continue; }
        DWORD n = 0;
        if (!ReadFile(g_audioRead, bufs[w].data(), AUDIO_CHUNK, &n, NULL) || n == 0)
            break;
        hdrs[w].dwBufferLength = n;
        if (waveOutPrepareHeader(wo, &hdrs[w], sizeof(hdrs[w])) == MMSYSERR_NOERROR &&
            waveOutWrite(wo, &hdrs[w], sizeof(hdrs[w])) == MMSYSERR_NOERROR) {
            inUse[w] = true;
        } else {
            inUse[w] = false;
        }
    }
    waveOutReset(wo);
    for (int i = 0; i < AUDIO_BUFS; ++i)
        if (hdrs[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(wo, &hdrs[i], sizeof(hdrs[i]));
    waveOutClose(wo);
    g_waveOut = NULL;
    return 0;
}

static void stopVideo() {
    InterlockedExchange(&g_running, 0);
    SetEvent(g_resumeEvent);
    if (g_ffmpegProc) {
        TerminateProcess(g_ffmpegProc, 0);
        CloseHandle(g_ffmpegProc);
        g_ffmpegProc = NULL;
    }
    if (g_audioProc) {
        TerminateProcess(g_audioProc, 0);
        CloseHandle(g_audioProc);
        g_audioProc = NULL;
    }
    if (g_workerThread) {
        WaitForSingleObject(g_workerThread, 3000);
        CloseHandle(g_workerThread);
        g_workerThread = NULL;
    }
    if (g_audioThread) {
        WaitForSingleObject(g_audioThread, 3000);
        CloseHandle(g_audioThread);
        g_audioThread = NULL;
    }
    if (g_pipeRead) {
        CloseHandle(g_pipeRead);
        g_pipeRead = NULL;
    }
    if (g_audioRead) {
        CloseHandle(g_audioRead);
        g_audioRead = NULL;
    }
    framesPlayed = 0;
}

static bool startPlayback(const std::wstring& file, double startSec) {
    stopVideo();
    HANDLE hRead = NULL;
    HANDLE hAudioRead = NULL;
    HANDLE proc = spawnFfmpeg(g_ffmpegPath, file, &hRead, startSec);
    if (!proc) return false;
    HANDLE aproc = spawnFfmpegAudio(g_ffmpegPath, file, &hAudioRead, startSec);
    g_pipeRead = hRead;
    g_ffmpegProc = proc;
    g_audioRead = hAudioRead;
    g_audioProc = aproc;
    EnterCriticalSection(&g_frameCs);
    g_simR.clear(); g_simG.clear(); g_simB.clear(); g_simCh.clear();
    g_simCols = 0; g_simRows = 0;
    g_raw.clear(); g_rawW = 0; g_rawH = 0;
    LeaveCriticalSection(&g_frameCs);
    InterlockedExchange(&g_hasFrame, 0);
    InterlockedExchange(&g_running, 1);
    InterlockedExchange(&g_paused, 0);
    SetEvent(g_resumeEvent);
    g_workerThread = CreateThread(NULL, 0, renderThread, NULL, 0, NULL);
    if (aproc && hAudioRead)
        g_audioThread = CreateThread(NULL, 0, audioThread, NULL, 0, NULL);
    InvalidateRect(g_hwnd, NULL, TRUE);
    return true;
}

static bool loadVideo(const std::wstring& file) {
    if (g_exporting) {
        MessageBoxW(g_hwnd, L"Please wait for the ASCII export to finish.",
            L"ASCII Player", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (g_ffprobePath.empty()) g_ffprobePath = findExe(L"ffprobe.exe");
    if (g_ffmpegPath.empty()) g_ffmpegPath = findExe(L"ffmpeg.exe");
    if (g_ffprobePath.empty() || g_ffmpegPath.empty()) {
        MessageBoxW(g_hwnd, L"ffmpeg/ffprobe not found on PATH.\nInstall with: winget install Gyan.FFmpeg",
            L"ASCII Player", MB_OK | MB_ICONERROR);
        return false;
    }
    VideoInfo vi = probeVideo(g_ffprobePath, file);
    if (vi.width <= 0 || vi.height <= 0) {
        MessageBoxW(g_hwnd, L"Could not read this video file.", L"ASCII Player", MB_OK | MB_ICONERROR);
        return false;
    }
    g_vi = vi;
    g_fileName = file;
    g_seekBase = 0.0;
    for (auto it = g_recent.begin(); it != g_recent.end();) {
        if (*it == file) it = g_recent.erase(it);
        else ++it;
    }
    g_recent.insert(g_recent.begin(), file);
    while (g_recent.size() > 8) g_recent.pop_back();
    size_t slash = file.find_last_of(L"\\/");
    if (slash != std::wstring::npos) g_lastDir = file.substr(0, slash + 1);
    saveSettings();
    rebuildFileMenu(GetMenu(g_hwnd));
    SetWindowTextW(g_hwnd, (std::wstring(L"ASCII Player - ") + file).c_str());
    startPlayback(file, 0.0);
    return true;
}

static void seekTo(double sec) {
    if (!g_ffmpegProc || g_fileName.empty()) return;
    if (g_vi.duration > 0) {
        if (sec < 0) sec = 0;
        if (sec > g_vi.duration) sec = g_vi.duration;
    }
    g_seekBase = sec;
    startPlayback(g_fileName, sec);
}

static void openDialog() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    if (!g_lastDir.empty()) ofn.lpstrInitialDir = g_lastDir.c_str();
    ofn.lpstrFilter = L"Video files\0*.mp4;*.avi;*.mov;*.mkv;*.webm;*.wmv;*.flv;*.m4v;*.ts;*.mpg\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) loadVideo(file);
}

static std::wstring downloadsDir() {
    PWSTR p = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, NULL, &p)) && p) {
        std::wstring dir = p;
        CoTaskMemFree(p);
        if (!dir.empty()) return dir;
    }
    wchar_t buf[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH) > 0) {
        std::wstring d = std::wstring(buf) + L"\\Downloads";
        CreateDirectoryW(d.c_str(), NULL);
        if (GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES) return d;
    }
    return L"";
}

static std::wstring randomName() {
    static const wchar_t* alpha = L"abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned int seed = (unsigned int)GetTickCount() ^ (unsigned int)GetCurrentProcessId()
        ^ ((unsigned int)(size_t)&alpha << 16);
    std::srand(seed);
    int n = (int)wcslen(alpha);
    wchar_t base[9];
    for (int i = 0; i < 8; ++i) base[i] = alpha[rand() % n];
    base[8] = 0;
    return std::wstring(base);
}

static DWORD WINAPI exportThread(LPVOID) {
    bool ok = false;
    std::wstring fail;
    do {
        if (g_ffmpegPath.empty()) { fail = L"ffmpeg not found."; break; }
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        int cw = rc.right - rc.left;
        int chH = rc.bottom - rc.top - STATUS_H;
        GridInfo gi = computeGrid(cw, chH);
        int cols = gi.cols, rows = gi.rows;
        int outW = cols * gi.cellW;
        int outH = rows * gi.cellH;
        if (outW < 2 || outH < 2) { fail = L"Bad export size."; break; }
        int W = g_vi.width, H = g_vi.height;
        double fps = g_vi.fps > 0 ? g_vi.fps : 24.0;

        HANDLE encWrite = NULL;
        std::vector<std::wstring> eargs;
        wchar_t sizeS[64], fpsS[32];
        swprintf(sizeS, 64, L"%dx%d", outW, outH);
        swprintf(fpsS, 32, L"%.3f", fps);
        eargs.push_back(L"-y"); eargs.push_back(L"-v"); eargs.push_back(L"error");
        eargs.push_back(L"-i"); eargs.push_back(g_fileName);
        eargs.push_back(L"-f"); eargs.push_back(L"rawvideo");
        eargs.push_back(L"-pix_fmt"); eargs.push_back(L"bgr24");
        eargs.push_back(L"-s"); eargs.push_back(sizeS);
        eargs.push_back(L"-r"); eargs.push_back(fpsS);
        eargs.push_back(L"-i"); eargs.push_back(L"pipe:0");
        eargs.push_back(L"-map"); eargs.push_back(L"1:v:0");
        eargs.push_back(L"-map"); eargs.push_back(L"0:a:0?");
        eargs.push_back(L"-c:v"); eargs.push_back(L"libx264");
        eargs.push_back(L"-preset"); eargs.push_back(L"veryfast");
        eargs.push_back(L"-crf"); eargs.push_back(L"18");
        eargs.push_back(L"-pix_fmt"); eargs.push_back(L"yuv420p");
        eargs.push_back(L"-c:a"); eargs.push_back(L"aac");
        eargs.push_back(L"-b:a"); eargs.push_back(L"160k");
        eargs.push_back(L"-shortest");
        eargs.push_back(L"-movflags"); eargs.push_back(L"+faststart");
        eargs.push_back(L"-bsf:v");
        eargs.push_back(L"h264_metadata=video_format=5:colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1:video_full_range_flag=0");
        eargs.push_back(g_exportPath);
        HANDLE encProc = spawnStdinProc(g_ffmpegPath, eargs, &encWrite);
        if (!encProc) { fail = L"Could not start the encoder."; break; }

        HANDLE decRead = NULL;
        HANDLE decProc = spawnFfmpeg(g_ffmpegPath, g_fileName, &decRead, 0.0);
        if (!decProc) {
            TerminateProcess(encProc, 0);
            CloseHandle(encProc);
            CloseHandle(encWrite);
            fail = L"Could not start the decoder.";
            break;
        }

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = outW;
        bi.bmiHeader.biHeight = -outH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 24;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = NULL;
        HDC screen = GetDC(NULL);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        ReleaseDC(NULL, screen);
        if (!dib || !bits) {
            CloseHandle(encWrite);
            CloseHandle(encProc);
            TerminateProcess(decProc, 0);
            CloseHandle(decProc);
            CloseHandle(decRead);
            fail = L"Could not allocate render surface.";
            break;
        }
        HGDIOBJ keepBmp = SelectObject(mem, dib);
        HFONT font = CreateFontW((int)(FONT_SIZES[g_fontIndex] * 1.4), 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
        HGDIOBJ keepFont = SelectObject(mem, font);
        TEXTMETRICW tm = {};
        GetTextMetricsW(mem, &tm);
        SetBkMode(mem, TRANSPARENT);
        SetTextCharacterExtra(mem, gi.cellW - tm.tmAveCharWidth);
        int stride = ((outW * 3) + 3) & ~3;
        RECT fullR = { 0, 0, outW, outH };

        size_t frameSize = (size_t)W * H * 3;
        std::vector<unsigned char> frame(frameSize);
        std::vector<unsigned char> r, g, b;
        std::vector<wchar_t> chs;
        std::vector<unsigned char> img((size_t)outH * outW * 3);
        long framesDone = 0;
        while (readExact(decRead, frame.data(), frameSize)) {
            buildAscii(frame.data(), W, H, cols, rows, r, g, b, chs);
            FillRect(mem, &fullR, (HBRUSH)GetStockObject(BLACK_BRUSH));
            int prevR = -2, prevG = -2, prevB = -2;
            int n = cols * rows;
            for (int i = 0; i < n; ++i) {
                int rr = r[i], gg = g[i], bb = b[i];
                if (rr != prevR || gg != prevG || bb != prevB) {
                    SetTextColor(mem, RGB(rr, gg, bb));
                    prevR = rr; prevG = gg; prevB = bb;
                }
                wchar_t one[2] = { chs[i], 0 };
                TextOutW(mem, (i % cols) * gi.cellW, (i / cols) * gi.cellH, one, 1);
            }
            GdiFlush();
            const unsigned char* src = (const unsigned char*)bits;
            for (int y = 0; y < outH; ++y)
                memcpy(img.data() + (size_t)y * outW * 3, src + (size_t)y * stride, (size_t)outW * 3);
            DWORD wrote = 0;
            if (!WriteFile(encWrite, img.data(), (DWORD)img.size(), &wrote, NULL)) break;
            ++framesDone;
        }
        CloseHandle(encWrite);
        if (WaitForSingleObject(encProc, 300000) != WAIT_OBJECT_0)
            TerminateProcess(encProc, 0);
        DWORD ec = 0;
        GetExitCodeProcess(encProc, &ec);
        CloseHandle(encProc);
        TerminateProcess(decProc, 0);
        CloseHandle(decProc);
        if (decRead) CloseHandle(decRead);
        SelectObject(mem, keepFont);
        DeleteObject(font);
        SelectObject(mem, keepBmp);
        DeleteObject(dib);
        DeleteDC(mem);
        ok = (ec == 0 && framesDone > 0);
        if (!ok) fail = L"Encoding failed (no frames).";
    } while (0);
    InterlockedExchange(&g_exporting, 0);
    PostMessageW(g_hwnd, WM_APP_EXPORT_DONE, ok ? 1 : 0, 0);
    return 0;
}

static void saveCopy() {
    if (g_fileName.empty()) return;
    if (InterlockedCompareExchange(&g_exporting, 1, 0)) {
        MessageBoxW(g_hwnd, L"An export is already in progress.", L"ASCII Player", MB_OK | MB_ICONINFORMATION);
        return;
    }
    size_t dot = g_fileName.find_last_of(L'.');
    std::wstring ext = (dot == std::wstring::npos) ? L".mp4" : g_fileName.substr(dot);
    if (ext.find_first_of(L"\\/") != std::wstring::npos) ext = L".mp4";
    std::wstring dest;
    for (int attempt = 0; attempt < 24; ++attempt) {
        dest = downloadsDir();
        if (dest.empty()) dest = L".";
        dest += L"\\" + randomName() + ext;
        HANDLE h = CreateFileW(dest.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) break;
        CloseHandle(h);
        dest.clear();
    }
    if (dest.empty()) {
        InterlockedExchange(&g_exporting, 0);
        MessageBoxW(g_hwnd, L"Could not find a free random file name.", L"ASCII Player", MB_OK | MB_ICONERROR);
        return;
    }
    g_exportPath = dest;
    SetWindowTextW(g_hwnd, L"ASCII Player - exporting...");
    g_exportThread = CreateThread(NULL, 0, exportThread, NULL, 0, NULL);
    if (g_exportThread) CloseHandle(g_exportThread);
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    case WM_DROPFILES: {
        wchar_t path[MAX_PATH];
        DragQueryFileW((HDROP)wp, 0, path, MAX_PATH);
        loadVideo(path);
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN: openDialog(); break;
        case IDM_SAVECOPY: saveCopy(); break;
        case IDM_PAUSE:
            if (g_running) {
                if (g_paused) {
                    InterlockedExchange(&g_paused, 0);
                    SetEvent(g_resumeEvent);
                } else {
                    InterlockedExchange(&g_paused, 1);
                }
                CheckMenuItem(GetMenu(hwnd), IDM_PAUSE, MF_BYCOMMAND | (g_paused ? MF_CHECKED : MF_UNCHECKED));
            }
            break;
        case IDM_ASPECT:
            InterlockedExchange(&g_aspect, g_aspect ? 0 : 1);
            CheckMenuItem(GetMenu(hwnd), IDM_ASPECT, MF_BYCOMMAND | (g_aspect ? MF_CHECKED : MF_UNCHECKED));
            break;
        case IDM_ZOOMIN:
            if (g_fontIndex < FONT_SIZES_N - 1) ++g_fontIndex;
            break;
        case IDM_ZOOMOUT:
            if (g_fontIndex > 0) --g_fontIndex;
            break;
        case IDM_INFO:
            InterlockedExchange(&g_showInfo, g_showInfo ? 0 : 1);
            CheckMenuItem(GetMenu(hwnd), IDM_INFO, MF_BYCOMMAND | (g_showInfo ? MF_CHECKED : MF_UNCHECKED));
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        case IDM_OPENWITH:
            installOpenWith();
            MessageBoxW(hwnd, L"\"ASCII Player\" was added to the Open with list for .mp4 files.\n\n"
                L"Right-click any .mp4 and choose Open with to see it.", L"ASCII Player",
                MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_REMOVEOPENWITH:
            uninstallOpenWith();
            MessageBoxW(hwnd, L"\"ASCII Player\" was removed from the Open with list.",
                L"ASCII Player", MB_OK | MB_ICONINFORMATION);
            break;
        default:
            if (LOWORD(wp) >= IDM_RECENT && LOWORD(wp) < IDM_RECENT + 8) {
                int idx = (int)LOWORD(wp) - IDM_RECENT;
                if (idx < (int)g_recent.size()) loadVideo(g_recent[idx]);
            }
            break;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if ((short)HIWORD(wp) > 0) {
            if (g_fontIndex < FONT_SIZES_N - 1) ++g_fontIndex;
        } else {
            if (g_fontIndex > 0) --g_fontIndex;
        }
        return 0;
    case WM_LBUTTONDOWN: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int x = (short)LOWORD(lp);
        int y = (short)HIWORD(lp);
        if (y >= rc.bottom - STATUS_H && g_vi.duration > 0 && g_ffmpegProc) {
            int bw = rc.right - rc.left - 16;
            if (bw > 8) {
                double frac = (double)(x - 8) / bw;
                if (frac < 0) frac = 0;
                if (frac > 1) frac = 1;
                seekTo(frac * g_vi.duration);
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_SPACE) SendMessageW(hwnd, WM_COMMAND, IDM_PAUSE, 0);
        else if (wp == VK_LEFT) seekTo(g_seekBase + (g_vi.fps > 0 ? (double)framesPlayed / g_vi.fps : 0.0) - 10.0);
        else if (wp == VK_RIGHT) seekTo(g_seekBase + (g_vi.fps > 0 ? (double)framesPlayed / g_vi.fps : 0.0) + 10.0);
        else if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        drawFrame(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_APP_FRAME:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_APP_EXPORT_DONE:
        if (!g_fileName.empty())
            SetWindowTextW(hwnd, (std::wstring(L"ASCII Player - ") + g_fileName).c_str());
        if (wp)
            MessageBoxW(hwnd, (std::wstring(L"Saved ASCII video to:\n") + g_exportPath).c_str(),
                L"ASCII Player", MB_OK | MB_ICONINFORMATION);
        else
            MessageBoxW(hwnd, L"Could not save the ASCII video.", L"ASCII Player", MB_OK | MB_ICONERROR);
        return 0;
    case WM_DESTROY:
        stopVideo();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR pCmdLine, int) {
    SetProcessDPIAware();
    SetUnhandledExceptionFilter(crashFilter);
    loadSettings();

    InitializeCriticalSection(&g_frameCs);
    g_resumeEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    SetEvent(g_resumeEvent);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"ASCII Player",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1024, 700,
        NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    g_hwnd = hwnd;

    HMENU menu = CreateMenu();
    rebuildFileMenu(menu);
    HMENU playM = CreatePopupMenu();
    AppendMenuW(playM, MF_STRING, IDM_PAUSE, L"&Play/Pause\tSpace");
    AppendMenuW(playM, MF_STRING, IDM_ASPECT, L"&Lock Aspect Ratio");
    AppendMenuW(playM, MF_SEPARATOR, 0, NULL);
    AppendMenuW(playM, MF_STRING, IDM_ZOOMIN, L"Zoom &In\tWheel up");
    AppendMenuW(playM, MF_STRING, IDM_ZOOMOUT, L"Zoom &Out\tWheel down");
    AppendMenuW(playM, MF_SEPARATOR, 0, NULL);
    AppendMenuW(playM, MF_STRING | MF_CHECKED, IDM_INFO, L"Show &Info");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)playM, L"&Play");
    HMENU toolsM = CreatePopupMenu();
    AppendMenuW(toolsM, MF_STRING, IDM_OPENWITH, L"Add to \"Open with\" (&mp4)");
    AppendMenuW(toolsM, MF_STRING, IDM_REMOVEOPENWITH, L"Remove from \"Open with\"");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)toolsM, L"&Tools");
    SetMenu(hwnd, menu);
    CheckMenuItem(menu, IDM_INFO, MF_BYCOMMAND | MF_CHECKED);

    ACCEL accel[3];
    accel[0].fVirt = FCONTROL | FVIRTKEY; accel[0].key = 'O'; accel[0].cmd = IDM_OPEN;
    accel[1].fVirt = FCONTROL | FVIRTKEY; accel[1].key = 'S'; accel[1].cmd = IDM_SAVECOPY;
    accel[2].fVirt = FVIRTKEY; accel[2].key = VK_SPACE; accel[2].cmd = IDM_PAUSE;
    HACCEL acc = CreateAcceleratorTableW(accel, 3);

    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    if (pCmdLine && pCmdLine[0]) {
        std::wstring cmd = pCmdLine;
        while (!cmd.empty() && (cmd[0] == L'"' || cmd[0] == L' ')) cmd = cmd.substr(1);
        while (!cmd.empty() && (cmd[cmd.size() - 1] == L'"' || cmd[cmd.size() - 1] == L' ')) cmd.pop_back();
        if (!cmd.empty()) loadVideo(cmd);
    } else {
        openDialog();
    }
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!TranslateAcceleratorW(hwnd, acc, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_bufDc) { DeleteDC(g_bufDc); g_bufDc = NULL; }
    if (g_bufBmp) { DeleteObject(g_bufBmp); g_bufBmp = NULL; }
    saveSettings();
    DeleteCriticalSection(&g_frameCs);
    CloseHandle(g_resumeEvent);
    return 0;
}