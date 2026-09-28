/* kineobridge_launcher.c -- KineoBridge.exe, the single customer-facing
 * entry point. Orchestrates: locate Kineo -> deploy private runtime
 * assets -> detect/repair camera USB/IP -> start the WSL bridge -> verify
 * bridge health -> launch Kineo (correct cwd, process-local env only) ->
 * verify it stays alive -> hand off to the normal Kineo UI.
 *
 * Design notes (see PROJECT_MEMORY.md for the full rationale):
 *   - Runs elevated (manifest: requireAdministrator, single UAC prompt
 *     at launch). This lets the launcher perform BOTH `usbipd bind`
 *     (first-time share, or after a Windows update resets share state)
 *     AND `usbipd attach` itself, with no manual admin command ever
 *     shown to the customer.
 *   - Never touches persistent User/Machine environment variables --
 *     GENICAM_GENTL64_PATH and KINEO_BRIDGE_SOURCE are set only in the
 *     explicit environment block passed to the one Kineo child process
 *     this launcher starts.
 *   - Never assumes success from CreateProcess alone -- Kineo's own
 *     ChironLog is polled for a real success/failure signal, matching
 *     the dev-tooling fix that caught a real false positive in
 *     tasklist-only polling.
 *   - All child-process interaction (usbipd.exe, wsl.exe, tasklist.exe)
 *     runs hidden (no console flash) with captured output; failures are
 *     surfaced as a short native MessageBox, never a console dump.
 */
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* ---- fixed configuration (matches every value proven throughout this
 * project's development -- see PROJECT_MEMORY.md) ---- */
#define CAMERA_VIDPID      "1409:8000"
#define CAMERA_SERIAL      "4110010861"
#define KINEO_INSTALL_DIR  "C:\\IMVapps\\Kineo Software"
#define KINEO_EXE_NAME     "Kineo Software.exe"
#define RUNTIME_DIR        "C:\\ProgramData\\KineoBridge\\runtime"
#define BRIDGE_PORT        "9494"
#define PHYSICAL_MAX_FPS   "60"

/* Deliberately hardcoded, not resolved dynamically (no "~", no
 * %USERPROFILE%/GetUserName() fallback): this build is tied to this
 * one specific machine on purpose (Windows account "IMV", WSL account
 * "imv"). Copying KineoBridge.exe/payload to any other account or
 * machine is expected to fail outright -- see the machine-lock check
 * in WinMain() and PROJECT_MEMORY.md's "Machine lock" note. */
#define WSL_HOME           "/home/imv"
#define WIN_LOCK_PATH      "C:\\Users\\IMV"

/* Hardware fingerprint lock (2026-09-28, deliberate, user-directed):
 * the WIN_LOCK_PATH check above only catches someone who didn't bother
 * to recreate the expected username/directories -- trivial to work
 * around by literally just imitating them on another machine. The real
 * backstop is a real ECDSA P-256 signature, not a plain hash:
 *
 *   1. get_hardware_fingerprint() queries "<motherboard serial>|<BIOS/
 *      system UUID>|<first disk serial>" via WMI, fresh at every launch.
 *   2. hw_sha256_hex() hashes it (Windows' own BCrypt API, no
 *      hand-rolled crypto).
 *   3. verify_hw_signature() verifies HW_SIGNATURE against that hash
 *      using the embedded HW_PUBKEY_X/Y (BCryptVerifySignature,
 *      ECDSA_P256) -- fails closed on any mismatch.
 *
 * HW_SIGNATURE was produced ONCE, offline, by signing this exact
 * machine's fingerprint with a private ECDSA P-256 key that was
 * generated in launcher/private/ (gitignored, never committed, never
 * embedded in the binary -- passphrase-encrypted at rest as an extra
 * layer). Only the PUBLIC key and the one resulting SIGNATURE are
 * embedded here; forging a signature for a different machine's
 * fingerprint would require that private key, not just reading this
 * source or disassembling the binary. This is meaningfully stronger
 * than the plain-hash-comparison this replaces: patching a hardcoded
 * expected hash to match a new fingerprint needs no secret at all,
 * but producing a new valid signature does. (Standard caveat that
 * applies to ANY client-side check, signature-based or not: someone
 * willing to binary-patch out the verification call entirely can
 * always do that -- no purely client-side check can prevent that.)
 * Fingerprint this was computed from (see PROJECT_MEMORY.md's
 * "Machine lock" note):
 *   A122221111B8602A|FDBB7861-D56E-FBA1-2371-B568E5A71537|E823_8FA6_BF53_0001_001B_448B_4DC2_A75C. */
static const unsigned char HW_PUBKEY_X[32] = {
    0x14, 0x9f, 0xc5, 0x90, 0xe0, 0x70, 0xbb, 0x5f, 0xa5, 0x92, 0x8d, 0x7e,
    0xaa, 0x12, 0x83, 0x20, 0xea, 0x50, 0xb0, 0xfc, 0x16, 0x54, 0x54, 0xa2,
    0x93, 0x12, 0x80, 0xc4, 0x9f, 0x86, 0xb3, 0x08,
};
static const unsigned char HW_PUBKEY_Y[32] = {
    0x89, 0xc0, 0x3d, 0x90, 0x13, 0x07, 0x75, 0x31, 0xfa, 0xc3, 0x68, 0x0f,
    0xb6, 0x18, 0xe8, 0x5e, 0xd9, 0xa1, 0x7f, 0x90, 0x88, 0x3b, 0x8d, 0x5a,
    0xae, 0xaf, 0x89, 0x5f, 0x66, 0xa5, 0xae, 0x48,
};
static const unsigned char HW_SIGNATURE[64] = {
    0x74, 0x7e, 0x7a, 0xdb, 0x68, 0xf3, 0x0f, 0x2a, 0xae, 0xa9, 0x04, 0x85,
    0x43, 0xdf, 0x5e, 0x27, 0x35, 0x87, 0x35, 0x20, 0x9e, 0xc7, 0x5b, 0x03,
    0xd2, 0xa2, 0x2d, 0xe9, 0x47, 0xc2, 0x50, 0xa3, 0xc3, 0xb0, 0x1f, 0xc7,
    0x84, 0x44, 0xe1, 0x47, 0x82, 0x0a, 0x27, 0xb6, 0x9e, 0x6e, 0xeb, 0xef,
    0x09, 0xce, 0x71, 0xbc, 0x70, 0xdf, 0x48, 0xf4, 0x70, 0xd9, 0xdb, 0x7d,
    0xf3, 0x63, 0x10, 0x19,
};
#define PRODUCT_NAME       "KineoBridge"
#define PRODUCT_VERSION    "1.0.0"
#define LOG_DIR            "C:\\ProgramData\\KineoBridge\\logs"
#define LOG_FILE           LOG_DIR "\\launcher.log"
#define LOG_MAX_BYTES      (2 * 1024 * 1024)

static HWND g_status_wnd = NULL;

/* ================= bounded diagnostic log =================
 * Never holds camera image data or proprietary source/protocol
 * dumps -- only launcher-side decisions (parsed USB/IP state, bridge
 * start/health, Kineo launch verification). Rotated once at 2MB so it
 * never grows unbounded across many launches. */
static void log_line(const char *fmt, ...) {
    CreateDirectoryA("C:\\ProgramData\\KineoBridge", NULL);
    CreateDirectoryA(LOG_DIR, NULL);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(LOG_FILE, GetFileExInfoStandard, &fad)) {
        ULONGLONG size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (size > LOG_MAX_BYTES) {
            char old[MAX_PATH];
            snprintf(old, sizeof old, "%s.old", LOG_FILE);
            DeleteFileA(old);
            MoveFileA(LOG_FILE, old);
        }
    }
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* ================= tiny status UI ================= */

#define IDC_TITLE          1
#define IDC_STATUS         2
#define IDC_MADE_WITH      3
#define IDC_LINK_WEBSITE   4
#define IDC_LINK_LINKEDIN  5

#define URL_WEBSITE  "https://siis.in"
#define URL_LINKEDIN "https://www.linkedin.com/in/eashaan-thakur/"

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_CTLCOLORSTATIC: {
            int id = GetDlgCtrlID((HWND)lp);
            HDC hdc = (HDC)wp;
            /* Title: lighter/medium gray -- de-emphasized, it barely
             * changes after the first glance. */
            if (id == IDC_TITLE) {
                SetTextColor(hdc, RGB(95, 95, 95));
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
            }
            /* Status: near-black -- this is the line that actually
             * matters moment to moment, it should be the most visible
             * text in the window, not the title. */
            if (id == IDC_STATUS) {
                SetTextColor(hdc, RGB(15, 15, 15));
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
            }
            /* "Made with (heart) by": small italic gray. */
            if (id == IDC_MADE_WITH) {
                SetTextColor(hdc, RGB(140, 140, 140));
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
            }
            /* SysLink attribution rows: this sets the color of the
             * PLAIN-text portion only -- SysLink renders its own
             * <A HREF> portion in the link color/underline regardless,
             * so "Website"/"LinkedIn" stay visually distinct as links
             * without any extra handling here. */
            if (id == IDC_LINK_WEBSITE || id == IDC_LINK_LINKEDIN) {
                SetTextColor(hdc, RGB(110, 110, 110));
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
            }
            return DefWindowProc(hwnd, msg, wp, lp);
        }
        case WM_NOTIFY: {
            /* NM_CLICK/NM_RETURN from a SysLink -- open its URL.
             * SysLink already shows a hand cursor over the link text
             * on its own, nothing to do for that here. */
            NMHDR *hdr = (NMHDR *)lp;
            if (hdr->code == NM_CLICK || hdr->code == NM_RETURN) {
                if (hdr->idFrom == IDC_LINK_WEBSITE)
                    ShellExecuteA(NULL, "open", URL_WEBSITE, NULL, NULL, SW_SHOWNORMAL);
                else if (hdr->idFrom == IDC_LINK_LINKEDIN)
                    ShellExecuteA(NULL, "open", URL_LINKEDIN, NULL, NULL, SW_SHOWNORMAL);
            }
            return 0;
        }
        default: return DefWindowProc(hwnd, msg, wp, lp);
    }
}

/* Creates a SysLink row, measures its OWN real ideal single-line size
 * on whatever machine this actually runs on (fonts/DPI vary -- this is
 * why it's measured at runtime instead of guessed at build time), and
 * centers it horizontally in a window of `win_w` pixels at height y. */
static void create_centered_link(HWND parent, HINSTANCE hinst, int ctrl_id,
                                  const char *markup, HFONT font, int win_w, int y) {
    HWND link = CreateWindowExA(0, "SysLink", markup,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 10, y, win_w - 20, 24,
        parent, (HMENU)(INT_PTR)ctrl_id, hinst, NULL);
    SendMessageA(link, WM_SETFONT, (WPARAM)font, TRUE);
    SIZE ideal = {0};
    /* mingw's commctrl.h only has the older LM_GETIDEALHEIGHT name --
     * same message, also fills in the width despite the name. */
    SendMessageA(link, LM_GETIDEALHEIGHT, (WPARAM)2000, (LPARAM)&ideal);
    if (ideal.cx > 0 && ideal.cx < win_w - 20) {
        SetWindowPos(link, NULL, (win_w - ideal.cx) / 2, y, ideal.cx, ideal.cy, SWP_NOZORDER);
    }
}

static HWND create_status_window(HINSTANCE hinst) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = "KineoBridgeLauncherWnd";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);

    const int WIN_W = 400, WIN_H = 190;
    HWND hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName,
        "Kineo Bridge", WS_POPUP | WS_BORDER | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, WIN_W, WIN_H, NULL, NULL, hinst, NULL);
    if (!hwnd) return NULL;

    /* Center on the primary monitor -- CW_USEDEFAULT doesn't reliably
     * center a small popup window on every Windows version. */
    RECT rc; GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd, NULL, (sx - w) / 2, (sy - h) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    /* Title + status are one visual group: title sits lighter/smaller
     * (medium gray, medium weight -- it barely changes, so it shouldn't
     * dominate), status sits right underneath with almost no gap,
     * bolder and near-black (it's the line that actually matters right
     * now). A comfortably larger gap then separates this whole group
     * from the footer below. */
    CreateWindowExA(0, "STATIC", "Kineo Bridge", WS_CHILD | WS_VISIBLE | SS_CENTER,
        10, 14, 380, 24, hwnd, (HMENU)IDC_TITLE, hinst, NULL);
    HFONT title_font = CreateFontA(-19, 0, 0, 0, FW_MEDIUM, 0, 0, 0, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Segoe UI");
    SendDlgItemMessageA(hwnd, IDC_TITLE, WM_SETFONT, (WPARAM)title_font, TRUE);

    CreateWindowExA(0, "STATIC", "Starting...", WS_CHILD | WS_VISIBLE | SS_CENTER,
        10, 40, 380, 32, hwnd, (HMENU)IDC_STATUS, hinst, NULL);
    HFONT status_font = CreateFontA(-16, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Segoe UI");
    SendDlgItemMessageA(hwnd, IDC_STATUS, WM_SETFONT, (WPARAM)status_font, TRUE);

    /* Thin inset separator, pushed well clear of the title/status group
     * above, then the attribution footer. */
    CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        20, 100, 360, 2, hwnd, NULL, hinst, NULL);

    /* "Made with <heart> by" -- a Unicode child window (created via the
     * -W API) is used only here so the heart glyph (U+2665) renders
     * correctly; every other control in this app stays on the ANSI ("A")
     * API to match the rest of the codebase. Classic GDI static text
     * can't do full-color emoji regardless of encoding, so this is the
     * plain heart symbol, not a colored emoji glyph. */
    HWND made_with = CreateWindowExW(0, L"STATIC", L"Made with \x2665 by",
        WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 110, 380, 18, hwnd,
        (HMENU)IDC_MADE_WITH, hinst, NULL);
    HFONT italic_font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, TRUE, 0, 0, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Segoe UI");
    SendMessageW(made_with, WM_SETFONT, (WPARAM)italic_font, TRUE);

    /* Two attribution rows: only "Website"/"LinkedIn" are the actual
     * clickable link (SysLink's <A HREF> markup), the rest is plain
     * text -- each row is measured and centered at its own real
     * rendered width, not a guessed one (see create_centered_link). */
    HFONT link_font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Segoe UI");
    create_centered_link(hwnd, hinst, IDC_LINK_WEBSITE,
        "System Integration and Infrastructure Solutions \xB7 <A HREF=\"" URL_WEBSITE "\">Website</A>",
        link_font, WIN_W, 130);
    create_centered_link(hwnd, hinst, IDC_LINK_LINKEDIN,
        "Eashaan Thakur \xB7 <A HREF=\"" URL_LINKEDIN "\">LinkedIn</A>",
        link_font, WIN_W, 150);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

static void set_status(const char *text) {
    if (!g_status_wnd) { printf("%s\n", text); return; }
    SetDlgItemTextA(g_status_wnd, IDC_STATUS, text);
    /* Pump the message queue so the label actually repaints -- this
     * launcher has no message loop of its own between steps. */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static void show_error(const char *title, const char *detail) {
    if (g_status_wnd) ShowWindow(g_status_wnd, SW_HIDE);
    MessageBoxA(NULL, detail, title, MB_OK | MB_ICONERROR);
}

/* ================= child-process helpers ================= */

/* Runs `cmdline` hidden, captures combined stdout+stderr into `out`
 * (bufcap bytes, NUL-terminated), waits up to timeout_ms, returns the
 * exit code (or -1 if the process couldn't be started/timed out). */
static int run_capture(const char *cmdline, char *out, size_t bufcap, DWORD timeout_ms) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE read_pipe, write_pipe;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return -1;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {0};
    char cmd_buf[4096];
    snprintf(cmd_buf, sizeof cmd_buf, "%s", cmdline);
    BOOL ok = CreateProcessA(NULL, cmd_buf, NULL, NULL, TRUE,
                              CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(write_pipe);
    if (!ok) { CloseHandle(read_pipe); return -1; }

    size_t total = 0;
    if (out) out[0] = '\0';
    char logbuf[1024]; size_t log_total = 0; logbuf[0] = '\0';
    for (;;) {
        char chunk[1024];
        DWORD n = 0;
        if (!ReadFile(read_pipe, chunk, sizeof(chunk) - 1, &n, NULL) || n == 0) break;
        chunk[n] = '\0';
        if (out && total + n < bufcap) {
            memcpy(out + total, chunk, n + 1);
            total += n;
        }
        if (log_total + n < sizeof(logbuf)) {
            memcpy(logbuf + log_total, chunk, n + 1);
            log_total += n;
        }
    }
    CloseHandle(read_pipe);

    DWORD wait_result = WaitForSingleObject(pi.hProcess, timeout_ms);
    DWORD exit_code = (DWORD)-1;
    if (wait_result == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &exit_code);
    else TerminateProcess(pi.hProcess, (UINT)-1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    log_line("run_capture: cmd=[%s] exit=%d output=[%s]", cmdline, (int)exit_code, logbuf);
    return (int)exit_code;
}

static int hw_sha256_raw(const char *data, size_t len, unsigned char digest[32]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return 0;
    if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
        if (BCryptHashData(hash, (PUCHAR)data, (ULONG)len, 0) == 0 &&
            BCryptFinishHash(hash, digest, 32, 0) == 0) {
            ok = 1;
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

#pragma pack(push, 1)
typedef struct { BCRYPT_ECCKEY_BLOB hdr; unsigned char x[32]; unsigned char y[32]; } hw_ecc_pub_blob_t;
#pragma pack(pop)

/* Verifies HW_SIGNATURE against SHA-256(fingerprint) using the
 * embedded HW_PUBKEY_X/Y (ECDSA P-256, Windows BCrypt API). Fails
 * closed: any error anywhere in this chain returns 0 (not verified). */
static int verify_hw_signature(const char *fingerprint) {
    unsigned char digest[32];
    if (!hw_sha256_raw(fingerprint, strlen(fingerprint), digest)) return 0;

    hw_ecc_pub_blob_t blob;
    blob.hdr.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.hdr.cbKey = 32;
    memcpy(blob.x, HW_PUBKEY_X, 32);
    memcpy(blob.y, HW_PUBKEY_Y, 32);

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) == 0) {
        if (BCryptImportKeyPair(alg, NULL, BCRYPT_ECCPUBLIC_BLOB, &key,
                                 (PUCHAR)&blob, sizeof blob, 0) == 0) {
            NTSTATUS st = BCryptVerifySignature(key, NULL, digest, sizeof digest,
                                                 (PUCHAR)HW_SIGNATURE, sizeof HW_SIGNATURE, 0);
            ok = (st == 0);
            BCryptDestroyKey(key);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
}

static int get_hardware_fingerprint(char *out, size_t outcap) {
    int rc = run_capture(
        "powershell.exe -NoProfile -Command \"$bb=(Get-CimInstance Win32_BaseBoard).SerialNumber; "
        "$bios=(Get-CimInstance Win32_ComputerSystemProduct).UUID; "
        "$disk=(Get-CimInstance Win32_DiskDrive | Select-Object -First 1).SerialNumber; "
        "Write-Output \\\"$bb|$bios|$disk\\\"\"",
        out, outcap, 10000);
    if (rc != 0 || !out) return 0;
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return n > 0;
}

static int process_is_running_by_name(const char *image_name) {
    char cmd[512], out[8192];
    snprintf(cmd, sizeof cmd, "tasklist.exe /FI \"IMAGENAME eq %s\"", image_name);
    if (run_capture(cmd, out, sizeof out, 5000) < 0) return 0;
    return strstr(out, image_name) != NULL;
}

/* ================= USB/IP (VID:PID discovered dynamically, never a
 * hardcoded BUSID -- see PROJECT_MEMORY.md, section 14) ================= */

typedef enum { USBIP_ATTACHED, USBIP_SHARED, USBIP_NOT_SHARED, USBIP_NOT_FOUND, USBIP_TOOL_MISSING } usbip_state_t;

static usbip_state_t usbip_find(char *busid_out, size_t busid_cap) {
    char out[8192];
    if (run_capture("where usbipd.exe", NULL, 0, 3000) != 0) return USBIP_TOOL_MISSING;
    if (run_capture("usbipd.exe list", out, sizeof out, 8000) < 0) return USBIP_TOOL_MISSING;

    /* Parse the "Connected:" section for our VID:PID; first column is
     * BUSID, state is whichever of these three substrings appears on
     * the same line. */
    char *connected = strstr(out, "Connected:");
    char *persisted = strstr(out, "Persisted:");
    if (!connected) return USBIP_NOT_FOUND;
    size_t seg_len = persisted ? (size_t)(persisted - connected) : strlen(connected);

    char *line = connected;
    while (line && (size_t)(line - connected) < seg_len) {
        char *eol = strchr(line, '\n');
        size_t linelen = eol ? (size_t)(eol - line) : strlen(line);
        char linebuf[512];
        size_t n = linelen < sizeof(linebuf) - 1 ? linelen : sizeof(linebuf) - 1;
        memcpy(linebuf, line, n); linebuf[n] = '\0';

        if (strstr(linebuf, CAMERA_VIDPID)) {
            /* Snapshot the full line BEFORE strtok mutates linebuf (it
             * writes a NUL at the first delimiter) -- the state check
             * below must see the whole line, not just the truncated
             * first token. Also bounds the state search to THIS line
             * only: searching the original `line` pointer instead would
             * scan into every subsequent line of the whole multi-line
             * usbipd output, and could match a DIFFERENT device's state
             * (this is a real bug that was found and fixed 2026-09-27:
             * a second, unrelated "Not shared" device listed right
             * after the camera's line caused the camera to be
             * misreported as NotShared even while genuinely Shared). */
            char full_line[512];
            strncpy(full_line, linebuf, sizeof full_line - 1);
            full_line[sizeof full_line - 1] = '\0';

            /* First whitespace-delimited token is the BUSID. */
            char *tok = strtok(linebuf, " \t");
            if (tok && busid_out) {
                strncpy(busid_out, tok, busid_cap - 1);
                busid_out[busid_cap - 1] = '\0';
            }
            usbip_state_t st;
            if (strstr(full_line, "Not shared")) st = USBIP_NOT_SHARED;
            else if (strstr(full_line, "Attached")) st = USBIP_ATTACHED;
            else if (strstr(full_line, "Shared")) st = USBIP_SHARED;
            else st = USBIP_NOT_FOUND;
            log_line("usbip_find: matched line=[%s] busid=[%s] state=%d",
                     full_line, busid_out ? busid_out : "", (int)st);
            return st;
        }
        line = eol ? eol + 1 : NULL;
    }
    log_line("usbip_find: VID:PID " CAMERA_VIDPID " not found in Connected: section");
    return USBIP_NOT_FOUND;
}

/* ================= private runtime payload deployment ================= */

/* Copies every file directly inside `src_dir` into `dst_dir` (creating
 * dst_dir if needed) -- one level, no recursion (both our payload
 * layouts here are flat: runtime\ and runtime\bridge\). Overwrites
 * unconditionally so an updated payload always wins. */
static void copy_dir_flat(const char *src_dir, const char *dst_dir) {
    CreateDirectoryA(dst_dir, NULL);
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", src_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char src[MAX_PATH], dst[MAX_PATH];
        snprintf(src, sizeof src, "%s\\%s", src_dir, fd.cFileName);
        snprintf(dst, sizeof dst, "%s\\%s", dst_dir, fd.cFileName);
        CopyFileA(src, dst, FALSE);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* Deploys this launcher's own payload (shipped in a `payload\` folder
 * next to KineoBridge.exe -- see scripts/package_release.sh) into
 * RUNTIME_DIR. Only the release .cti/.xml and the compiled (.pyc)
 * bridge files ever ship here -- no .py/.c source, per the private-
 * runtime requirement. Safe to call every launch: CopyFileA
 * unconditionally overwrites, so an updated payload always takes
 * effect and there's no separate "version check" to get wrong. */
static int deploy_runtime_payload(void) {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, sizeof exe_path);
    char *last_slash = strrchr(exe_path, '\\');
    if (last_slash) *last_slash = '\0';

    char payload_dir[MAX_PATH], payload_bridge_dir[MAX_PATH];
    snprintf(payload_dir, sizeof payload_dir, "%s\\payload", exe_path);
    snprintf(payload_bridge_dir, sizeof payload_bridge_dir, "%s\\payload\\bridge", exe_path);
    if (GetFileAttributesA(payload_dir) == INVALID_FILE_ATTRIBUTES) {
        /* No payload next to the exe -- fine during development (the
         * dev-tooling RUNTIME_DIR may already be populated by hand);
         * a real release build always ships one. */
        return 1;
    }

    CreateDirectoryA("C:\\ProgramData\\KineoBridge", NULL);
    CreateDirectoryA(RUNTIME_DIR, NULL);
    copy_dir_flat(payload_dir, RUNTIME_DIR);
    char runtime_bridge_dir[MAX_PATH];
    snprintf(runtime_bridge_dir, sizeof runtime_bridge_dir, "%s\\bridge", RUNTIME_DIR);
    copy_dir_flat(payload_bridge_dir, runtime_bridge_dir);
    return 1;
}

static int wsl_run(const char *bash_cmd, char *out, size_t outcap, DWORD timeout_ms) {
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "wsl.exe -- bash -lc \"%s\"", bash_cmd);
    return run_capture(cmd, out, outcap, timeout_ms);
}

static int wsl_sees_camera(void) {
    char out[4096];
    int rc = wsl_run("lsusb", out, sizeof out, 5000);
    return rc == 0 && strstr(out, "1409:8000") != NULL;
}

static int aravis_sees_camera(void) {
    /* Reuses the exact same private runtime the bridge itself uses --
     * see deploy_runtime()/GI_TYPELIB_PATH below. */
    char out[512];
    int rc = wsl_run(
        "GI_TYPELIB_PATH=" WSL_HOME "/aravis-0.8.36/build/src LD_LIBRARY_PATH=" WSL_HOME "/aravis-0.8.36/build/src "
        "python3 -c \\\"import gi; gi.require_version('Aravis','0.8'); from gi.repository import Aravis; "
        "Aravis.update_device_list(); import sys; sys.exit(0 if any(Aravis.get_device_serial_nbr(i)=='"
        CAMERA_SERIAL "' for i in range(Aravis.get_n_devices())) else 1)\\\"",
        out, sizeof out, 8000);
    return rc == 0;
}

/* Bounded recovery: bind (if NotShared) and/or attach (if Shared-but-
 * not-Attached). The launcher runs elevated (manifest:
 * requireAdministrator), so both operations are available without any
 * manual admin command ever being shown to the customer. */
static int ensure_camera_ready(void) {
    for (int attempt = 0; attempt < 5; attempt++) {
        char busid[64] = "";
        usbip_state_t state = usbip_find(busid, sizeof busid);
        log_line("ensure_camera_ready: attempt=%d state=%d busid=%s", attempt, (int)state, busid);
        switch (state) {
            case USBIP_TOOL_MISSING:
                show_error("Kineo Bridge", "usbipd-win is not installed or not on PATH.\n\n"
                    "Install usbipd-win, then run KineoBridge again.\n\nError KB-USB-000");
                return 0;
            case USBIP_NOT_FOUND:
                show_error("Kineo Bridge", "Camera not detected.\n\n"
                    "Check the USB connection, then run KineoBridge again.\n\nError KB-USB-001");
                return 0;
            case USBIP_NOT_SHARED: {
                char cmd[128];
                snprintf(cmd, sizeof cmd, "usbipd.exe bind --busid %s", busid);
                set_status("Preparing camera (one-time setup)...");
                run_capture(cmd, NULL, 0, 8000);
                /* re-check next loop iteration; if bind didn't take,
                 * the bounded retry below eventually reports failure. */
                break;
            }
            case USBIP_ATTACHED: {
                int wsl_ok = wsl_sees_camera();
                int aravis_ok = wsl_ok && aravis_sees_camera();
                log_line("ensure_camera_ready: ATTACHED, wsl_sees_camera=%d aravis_sees_camera=%d",
                          wsl_ok, aravis_ok);
                if (wsl_ok && aravis_ok) return 1;
                /* fall through to retry -- enumeration lag */
                break;
            }
            case USBIP_SHARED: {
                char cmd[128];
                snprintf(cmd, sizeof cmd, "usbipd.exe attach --wsl --busid %s", busid);
                run_capture(cmd, NULL, 0, 8000);
                break;
            }
        }
        Sleep(1000 * (attempt + 1));
    }
    show_error("Kineo Bridge", "Camera connection failed.\n\n"
        "Try unplugging and reconnecting the camera, then run KineoBridge again.\n\nError KB-USB-003");
    return 0;
}

/* ================= bridge lifecycle ================= */

static int deploy_and_start_bridge(void) {
    /* Deploy this launcher's private runtime payload (compiled .pyc --
     * see payload/bridge/) into a private WSL-side path, then start it.
     * RUNTIME_DIR (Windows-visible) is where THIS launcher's payload
     * directory lives; it's copied into WSL via the /mnt/c bridge so no
     * binary data has to be piped through wsl.exe's stdin. */
    /* Stop any previous instance via a PID file, never `pkill -f
     * <pattern>` -- a real bug (found 2026-09-27): this whole command
     * is itself invoked as `bash -lc "<this string>"`, and that string
     * contains the literal text "kineo_camera_bridge.pyc" (in both the
     * kill step and the nohup step below) -- so a pattern-based
     * pkill/pgrep for that same text run FROM WITHIN this invocation
     * matches its own invoking shell's process command line and kills
     * it before the script ever reaches echo STARTED, silently. This
     * is the same class of self-matching pkill bug documented
     * elsewhere in this project's dev tooling, see PROJECT_MEMORY.md.
     * PID-file based kill has no such self-match risk.
     *
     * The `cd` must be its OWN statement (not `cd dir && ... &`) --
     * backgrounding a `&&`-joined compound list makes bash fork a
     * subshell to run the whole list, and `$!` then captures THAT
     * subshell's PID, not python3's real PID (verified directly: with
     * `cd dir && env... nohup python3 ... &`, `$!` and the actual
     * running python3 PID were two different, both-alive processes;
     * killing the `$!` one would leave python3 -- protected by nohup
     * from the resulting SIGHUP -- running forever). With `cd` as a
     * separate prior statement, the backgrounded command is a single
     * simple command, which avoids that particular subshell trap.
     *
     * BUT: `$!` itself was found to be unreliable through the
     * `wsl.exe -- bash -lc "..."` interop path in this environment
     * (found 2026-09-27, investigating a KB-USB-003 report that turned
     * out to be unrelated to camera/USB at all): a minimal, fully
     * isolated repro -- `wsl.exe -- bash -c 'sleep 5 & echo $!'`, run
     * from a genuine native Windows process, no shell tricks -- printed
     * an EMPTY PID every time. The real, shipped `bridge.pid` matched
     * this exactly: a bare newline, no digits, meaning
     * `stop_bridge()`'s `kill "$(cat bridge.pid)"` had been silently a
     * no-op. Fixed by never relying on `$!` at all: the backgrounded
     * command is now `bash -c 'echo $$ > bridge.pid; exec python3 ...'`
     * -- `$$` here is the *new* bash's own PID, read from inside
     * itself, and since `exec` replaces that process in place (same
     * PID, no further fork), the PID written is guaranteed to be
     * python3's real PID by construction, with no dependency on the
     * outer shell's job-table bookkeeping at all. Verified directly
     * across several clean runs (each cross-checked against an
     * independent `ps` snapshot) before shipping this. */
    /* Delete any stale status file from a previous run BEFORE starting
     * the new process -- a real bug (found 2026-09-27, sixth live
     * launch attempt): the health check below only tested for the
     * status file's EXISTENCE, and a leftover file from an earlier run
     * (this exact scenario happened during this project's own dev
     * testing) made the check report healthy on its very first poll
     * even though the freshly-started process had not actually written
     * anything yet -- masking a real startup failure (Kineo later got
     * WSAECONNREFUSED trying to reach the bridge that never came up).
     * Deleting it first means "the file exists" can only become true
     * again once THIS run's process writes a fresh one. */
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
        "mkdir -p " WSL_HOME "/.local/lib/kineobridge && "
        "cp /mnt/c/ProgramData/KineoBridge/runtime/bridge/*.pyc " WSL_HOME "/.local/lib/kineobridge/ 2>/dev/null; "
        "if [ -f " WSL_HOME "/.local/lib/kineobridge/bridge.pid ]; then "
        "kill \"$(cat " WSL_HOME "/.local/lib/kineobridge/bridge.pid)\" 2>/dev/null; fi; sleep 1; "
        "rm -f " WSL_HOME "/.local/lib/kineobridge/bridge_status." BRIDGE_PORT ".json; "
        "cd " WSL_HOME "/.local/lib/kineobridge; "
        "GI_TYPELIB_PATH=" WSL_HOME "/aravis-0.8.36/build/src LD_LIBRARY_PATH=" WSL_HOME "/aravis-0.8.36/build/src "
        "KINEO_BRIDGE_MAX_FPS=" PHYSICAL_MAX_FPS " "
        /* setsid fully detaches the process from this wsl.exe
         * invocation's own session -- a real bug (found 2026-09-27,
         * seventh live launch attempt, KB-BRIDGE-001): this whole
         * command is one-shot (`wsl.exe -- bash -lc "..."`), and WSL
         * tears down the entire session belonging to that one-shot
         * client the moment it exits, killing any descendant process
         * still in that session -- including one started with
         * `nohup ... & disown`, since those only protect against
         * SIGHUP and bash's own job-table tracking, not against WSL's
         * session-level cleanup. Verified directly: the exact same
         * command survives when run inside an already-open, persistent
         * WSL shell, but reliably dies within ~1-2s when run via a
         * fresh `wsl.exe -- bash -lc` invocation, UNLESS `setsid` is
         * used (confirmed the process's own session leadership via
         * `ps` showing state `Ssl`) AND the script sleeps briefly
         * after backgrounding so the fork/setsid genuinely completes
         * before this one-shot session ends -- without that sleep,
         * `setsid` alone was still not sufficient (also verified
         * directly). */
        "setsid nohup bash -c 'echo $$ > " WSL_HOME "/.local/lib/kineobridge/bridge.pid; "
        "exec python3 -u kineo_camera_bridge.pyc --source camera --host 0.0.0.0 --port " BRIDGE_PORT "' "
        "> /tmp/kineobridge.log 2>&1 < /dev/null & "
        "disown; sleep 1; echo STARTED");
    char out[256];
    if (wsl_run(cmd, out, sizeof out, 8000) < 0 || !strstr(out, "STARTED")) return 0;

    /* Health check: read the bridge's own status file back through the
     * same /mnt/c path convention this whole project uses. Also require
     * at least 2 seconds elapsed (i > 3, since each iteration sleeps
     * 500ms) before accepting success -- a real crash-on-startup could
     * still write a status file within the first poll or two before
     * dying, so a couple of confirming polls make a fluke far less
     * likely to be reported as healthy. */
    for (int i = 0; i < 20; i++) {
        Sleep(500);
        char probe_out[512];
        int rc = wsl_run("test -f " WSL_HOME "/.local/lib/kineobridge/bridge_status." BRIDGE_PORT ".json && echo OK",
                          probe_out, sizeof probe_out, 3000);
        if (rc == 0 && strstr(probe_out, "OK") && i >= 3) return 1;
    }
    return 0;
}

static void stop_bridge(void) {
    /* PID-file based, not `pkill -f` -- see the comment in
     * deploy_and_start_bridge() for why a pattern-based kill here would
     * self-match this very command's own invoking shell. */
    wsl_run("if [ -f " WSL_HOME "/.local/lib/kineobridge/bridge.pid ]; then "
            "kill \"$(cat " WSL_HOME "/.local/lib/kineobridge/bridge.pid)\" 2>/dev/null; fi",
            NULL, 0, 3000);
}

/* ================= Kineo launch ================= */

#define GENTL_KEY  "GENICAM_GENTL64_PATH="
#define SOURCE_KEY "KINEO_BRIDGE_SOURCE="

/* Builds a child environment block = current environment + our two
 * process-local overrides, for CreateProcess's lpEnvironment. Never
 * touches the real (persistent) environment.
 *
 * Real bug (found 2026-09-27, fifth live launch attempt, Kineo
 * disconnected mid-analysis): this machine has a pre-existing
 * MACHINE-level GENICAM_GENTL64_PATH (pointing at Kineo's own bundled
 * vendor IDS directory), which GetEnvironmentStringsA() inherits into
 * `base`. The old code only ever APPENDED its own
 * GENICAM_GENTL64_PATH/KINEO_BRIDGE_SOURCE entries after the full
 * copied base block, leaving a DUPLICATE key in the environment block
 * -- which of the two same-named entries ids_peak.dll's GenTL producer
 * scan actually honors is unspecified/inconsistent, and evidence
 * pointed at it not reliably being ours. Fixed properly: drop any
 * existing entries for these two names from the copied base block, and
 * insert our own single, authoritative values -- with our runtime dir
 * listed FIRST in a semicolon-list ahead of whatever the machine value
 * already was, so the genuine vendor CTI directory stays reachable
 * (matches the original design intent: "prepend private runtime dir to
 * existing Machine value") but our producer is always found first,
 * unambiguously, with no duplicate key. */
static char *build_kineo_env(size_t *out_len) {
    char *base = GetEnvironmentStringsA();
    size_t gentl_key_len = strlen(GENTL_KEY);
    size_t source_key_len = strlen(SOURCE_KEY);

    char old_gentl[1024] = "";
    size_t kept_len = 0;
    for (char *p = base; *p; ) {
        size_t l = strlen(p) + 1;
        int is_gentl = _strnicmp(p, GENTL_KEY, gentl_key_len) == 0;
        int is_source = _strnicmp(p, SOURCE_KEY, source_key_len) == 0;
        if (is_gentl) {
            strncpy(old_gentl, p + gentl_key_len, sizeof old_gentl - 1);
            old_gentl[sizeof old_gentl - 1] = '\0';
        }
        if (!is_gentl && !is_source) kept_len += l;
        p += l;
    }
    kept_len += 1; /* final double-NUL terminator */

    const char *extra1 = SOURCE_KEY "wsl";
    char extra2[sizeof(old_gentl) + MAX_PATH + gentl_key_len + 8];
    if (old_gentl[0]) {
        snprintf(extra2, sizeof extra2, "%s%s;%s", GENTL_KEY, RUNTIME_DIR, old_gentl);
    } else {
        snprintf(extra2, sizeof extra2, "%s%s", GENTL_KEY, RUNTIME_DIR);
    }
    size_t extra_len = strlen(extra1) + 1 + strlen(extra2) + 1;

    char *buf = (char *)malloc(kept_len + extra_len);
    size_t off = 0;
    for (char *p = base; *p; ) {
        size_t l = strlen(p) + 1;
        int is_gentl = _strnicmp(p, GENTL_KEY, gentl_key_len) == 0;
        int is_source = _strnicmp(p, SOURCE_KEY, source_key_len) == 0;
        if (!is_gentl && !is_source) { memcpy(buf + off, p, l); off += l; }
        p += l;
    }
    memcpy(buf + off, extra1, strlen(extra1) + 1); off += strlen(extra1) + 1;
    memcpy(buf + off, extra2, strlen(extra2) + 1); off += strlen(extra2) + 1;
    buf[off] = '\0'; off += 1;
    FreeEnvironmentStringsA(base);
    log_line("build_kineo_env: old GENICAM_GENTL64_PATH=[%s] new value=[%s]", old_gentl, extra2);
    *out_len = off;
    return buf;
}

static int find_chiron_log(char *path_out, size_t cap) {
    /* Newest ChironLog_*.log under Kineo's own log directory. */
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\Data\\Logs\\Application\\ChironLog_*.log", KINEO_INSTALL_DIR);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILETIME newest = {0};
    char best[MAX_PATH] = "";
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &newest) >= 0) {
            newest = fd.ftLastWriteTime;
            strncpy(best, fd.cFileName, sizeof best - 1);
            best[sizeof best - 1] = '\0';
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (!best[0]) return 0;
    snprintf(path_out, cap, "%s\\Data\\Logs\\Application\\%s", KINEO_INSTALL_DIR, best);
    return 1;
}

static long chiron_log_line_count(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    long count = 0;
    int c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') count++;
    fclose(f);
    return count;
}

/* Reads lines strictly after `from_line` in the ChironLog and checks for
 * a known success/failure marker -- far more reliable than OS process
 * polling (see PROJECT_MEMORY.md for the false-positive this replaced). */
static int chiron_log_check(const char *path, long from_line, int *out_ok) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[2048];
    long n = 0;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        n++;
        if (n <= from_line) continue;
        if (strstr(line, "Device component cannot start") || strstr(line, "Child process exited with code")) {
            *out_ok = 0; found = 1;
        } else if (strstr(line, "App already running, quitting second instance") ||
                   strstr(line, "WebSocket connection established")) {
            *out_ok = 1; found = 1;
        }
    }
    fclose(f);
    return found;
}

static int launch_and_verify_kineo(void) {
    char chiron_path[MAX_PATH] = "";
    find_chiron_log(chiron_path, sizeof chiron_path);

    for (int attempt = 1; attempt <= 2; attempt++) {
        /* Real bug, found 2026-09-28 (a genuine Kineo-side crash,
         * STATUS_STACK_BUFFER_OVERRUN in KineoDeviceService.exe, right
         * after its own log warned "Running KineoDeviceService
         * processus found" -- i.e. a leftover, not-yet-cleaned-up
         * instance): previously `mark` was computed ONCE before this
         * loop, so a retry's ChironLog scan window still included the
         * PREVIOUS attempt's own failure line -- attempt 2 would see
         * attempt 1's stale error and report failure almost instantly
         * (observed: ~1s, far too fast to be a real signal from attempt
         * 2's own process), never giving attempt 2 a fair chance. Also,
         * nothing killed attempt 1's process tree before attempt 2
         * launched a SECOND "Kineo Software.exe" on top of it -- the
         * likely real cause of the orphaned KineoDeviceService.exe.
         * Fixed: re-mark fresh before every attempt, and clean up any
         * previous attempt's processes first. */
        if (attempt > 1) {
            run_capture("taskkill.exe /IM \"" KINEO_EXE_NAME "\" /F", NULL, 0, 5000);
            run_capture("taskkill.exe /IM \"KineoDeviceService.exe\" /F", NULL, 0, 5000);
            Sleep(1500);
            find_chiron_log(chiron_path, sizeof chiron_path);
        }
        long mark = chiron_path[0] ? chiron_log_line_count(chiron_path) : 0;
        log_line("launch_and_verify_kineo: attempt=%d chiron_path=[%s] mark=%ld",
                  attempt, chiron_path, mark);

        size_t env_len; char *env = build_kineo_env(&env_len);
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi = {0};
        char cmdline[MAX_PATH + 32];
        snprintf(cmdline, sizeof cmdline, "\"%s\\%s\"", KINEO_INSTALL_DIR, KINEO_EXE_NAME);
        /* CREATE_UNICODE_ENVIRONMENT must NOT be set here -- a real bug
         * (found 2026-09-27, fourth live launch attempt): build_kineo_env()
         * builds a plain ANSI (narrow) environment block via
         * GetEnvironmentStringsA(), but that flag tells CreateProcessA to
         * interpret lpEnvironment as UTF-16, corrupting every variable
         * the child process sees (including PATH, APPDATA, and our own
         * GENICAM_GENTL64_PATH/KINEO_BRIDGE_SOURCE) even when
         * CreateProcessA itself still reports success. */
        BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                                  CREATE_NEW_CONSOLE,
                                  env, KINEO_INSTALL_DIR, &si, &pi);
        DWORD create_err = ok ? 0 : GetLastError();
        free(env);
        if (ok) {
            log_line("launch_and_verify_kineo: attempt=%d CreateProcessA OK pid=%lu",
                      attempt, (unsigned long)pi.dwProcessId);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        } else {
            log_line("launch_and_verify_kineo: attempt=%d CreateProcessA FAILED GetLastError=%lu",
                      attempt, (unsigned long)create_err);
        }

        int result_ok = -1;
        for (int i = 0; i < 20; i++) {
            Sleep(1000);
            if (!chiron_path[0]) find_chiron_log(chiron_path, sizeof chiron_path);
            if (chiron_path[0]) {
                int ok_flag = 0;
                if (chiron_log_check(chiron_path, mark, &ok_flag)) { result_ok = ok_flag; break; }
            }
        }
        log_line("launch_and_verify_kineo: attempt=%d result_ok=%d chiron_path=[%s]",
                  attempt, result_ok, chiron_path);
        if (result_ok == 1) return 1;
        char msg[128];
        snprintf(msg, sizeof msg, "attempt %d: Kineo did not start cleanly, retrying...", attempt);
        set_status(msg);
    }
    return 0;
}

/* ================= main ================= */

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE hprev, LPSTR cmdline, int nshow) {
    (void)hprev; (void)cmdline; (void)nshow;
    log_line("=== KineoBridge %s starting ===", PRODUCT_VERSION);

    /* Machine lock (deliberate, see WSL_HOME/WIN_LOCK_PATH above): this
     * build is tied to one specific machine/account on purpose, not
     * meant to run correctly if the exe/payload is copied elsewhere.
     * Checked before anything else -- including creating the status
     * window -- so a copy elsewhere fails immediately rather than
     * partway through camera/bridge setup. */
    DWORD lock_attrs = GetFileAttributesA(WIN_LOCK_PATH);
    if (lock_attrs == INVALID_FILE_ATTRIBUTES || !(lock_attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        log_line("machine lock: " WIN_LOCK_PATH " not found, refusing to start");
        MessageBoxA(NULL,
            "Kineo Bridge could not start on this machine.\n\n"
            "Error KB-ENV-001",
            "Kineo Bridge", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Hardware fingerprint lock (see HW_PUBKEY_X/Y/HW_SIGNATURE above)
     * -- the real backstop, checked after the cheap path check so a
     * wrong machine fails fast without spawning PowerShell/WMI in the
     * common case, but still before the status window. */
    {
        char fp[512];
        int fp_ok = get_hardware_fingerprint(fp, sizeof fp);
        int match = fp_ok && verify_hw_signature(fp);
        log_line("hw lock: fp_ok=%d match=%d", fp_ok, match);
        if (!match) {
            MessageBoxA(NULL,
                "Kineo Bridge could not start on this machine.\n\n"
                "Error KB-ENV-002",
                "Kineo Bridge", MB_OK | MB_ICONERROR);
            return 1;
        }
    }

    g_status_wnd = create_status_window(hinst);

    set_status("Checking Kineo installation...");
    char kineo_exe_path[MAX_PATH];
    snprintf(kineo_exe_path, sizeof kineo_exe_path, "%s\\%s", KINEO_INSTALL_DIR, KINEO_EXE_NAME);
    if (GetFileAttributesA(kineo_exe_path) == INVALID_FILE_ATTRIBUTES) {
        show_error("Kineo Bridge", "Kineo installation not found.\n\n"
                   "Install Kineo Software, then run KineoBridge again.\n\nError KB-KINEO-001");
        return 1;
    }

    if (process_is_running_by_name(KINEO_EXE_NAME)) {
        set_status("Closing a previous Kineo session...");
        run_capture("taskkill.exe /IM \"" KINEO_EXE_NAME "\" /F", NULL, 0, 5000);
        Sleep(1000);
    }

    set_status("Preparing camera bridge runtime...");
    deploy_runtime_payload();

    set_status("Checking camera...");
    if (!ensure_camera_ready()) return 1;

    set_status("Connecting camera...");
    if (!deploy_and_start_bridge()) {
        show_error("Kineo Bridge", "Camera bridge failed to start.\n\n"
            "Try running KineoBridge again. If this keeps happening, contact support.\n\nError KB-BRIDGE-001");
        return 1;
    }

    set_status("Starting Kineo...");
    if (!launch_and_verify_kineo()) {
        stop_bridge();
        show_error("Kineo Bridge", "Kineo failed to start.\n\n"
            "This is usually a temporary Windows-side issue. Try running KineoBridge again.\n\nError KB-KINEO-002");
        return 1;
    }

    set_status("Kineo is running.");
    Sleep(1200);
    if (g_status_wnd) DestroyWindow(g_status_wnd);
    return 0;
}
