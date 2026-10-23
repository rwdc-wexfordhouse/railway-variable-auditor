/*
 * Copyright (c) 2026, Ross WD Cameron <ross@wexfordhouse.co.za>
 * Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
 *
 * Wizard installer and uninstaller for Railway Variable Auditor, built on the classic Win32
 * PropertySheet wizard (no installer framework). Adapted from xdg-open-win.
 *
 *   setup.exe                     wizard
 *   setup.exe /S [/D=dir]         silent install (also --silent, --dir <dir>)
 *   uninstall.exe                 uninstall (asks first; --quiet to skip the question)
 */
#include <windows.h>
#include <commctrl.h>
#include <prsht.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "common.h"
#include "install.h"
#include "resource.h"

#ifndef ID_WIZNEXT
#define ID_WIZNEXT 0x3024
#endif
#define WM_INSTALL_PROGRESS (WM_APP + 1)
#define WM_INSTALL_DONE     (WM_APP + 2)

static InstallOptions g_opt;
static BOOL g_license_accepted = FALSE;
static wchar_t *g_license_text = NULL;
static BOOL g_install_ok = FALSE;
static BOOL g_installing = FALSE;
static BOOL g_started = FALSE;
static wchar_t g_error[512];

/* ---------- licence text from the payload ---------- */

static BOOL find_license(const char *path, const BYTE *data, uint64_t size, void *ctx)
{
    (void)ctx;
    if (strcmp(path, LICENSE_ENTRY) != 0)
        return TRUE;
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)data, (int)size, NULL, 0);
        wchar_t *w = malloc(((size_t)n * 2 + 1) * sizeof *w);
        wchar_t *out = w;
        wchar_t *tmp = malloc(((size_t)n + 1) * sizeof *tmp);
        if (!w || !tmp) {
            free(w);
            free(tmp);
            return FALSE;
        }
        MultiByteToWideChar(CP_UTF8, 0, (const char *)data, (int)size, tmp, n);
        tmp[n] = 0;
        for (wchar_t *s = tmp; *s; s++) { /* the edit control needs CRLF */
            if (*s == L'\n' && (s == tmp || s[-1] != L'\r'))
                *out++ = L'\r';
            *out++ = *s;
        }
        *out = 0;
        free(tmp);
        g_license_text = w;
    }
    return FALSE; /* stop walking */
}

static void load_license(void)
{
    Payload p;
    wchar_t err[128];
    if (payload_open(&p, FALSE, err, 128)) {
        payload_walk(&p, TRUE, find_license, NULL, err, 128);
        payload_close(&p);
    }
}

/* ---------- wizard pages ---------- */

static void buttons(HWND page, DWORD flags)
{
    PropSheet_SetWizButtons(GetParent(page), flags);
}

static int CALLBACK browse_cb(HWND hwnd, UINT msg, LPARAM lp, LPARAM data)
{
    (void)lp;
    if (msg == BFFM_INITIALIZED)
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

static INT_PTR CALLBACK WelcomeProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)w;
    if (m == WM_NOTIFY && ((NMHDR *)l)->code == PSN_SETACTIVE) {
        buttons(h, PSWIZB_NEXT);
        SetWindowLongPtrW(h, DWLP_MSGRESULT, 0);
        return TRUE;
    }
    return FALSE;
}

#define WM_LICENSE_FOCUS (WM_APP + 3)

/* The dialog manager selects all the text of an edit box that reports DLGC_HASSETSEL whenever
 * focus moves into it (Tab, or the wizard choosing its first control). Hide that flag so the
 * licence is never shown highlighted. The box can still be reached and scrolled with the keyboard. */
static LRESULT CALLBACK no_select_all(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    if (m == WM_GETDLGCODE)
        return DefSubclassProc(h, m, w, l) & ~(LRESULT)DLGC_HASSETSEL;
    if (m == WM_NCDESTROY)
        RemoveWindowSubclass(h, no_select_all, id);
    return DefSubclassProc(h, m, w, l);
}

static INT_PTR CALLBACK LicenseProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_INITDIALOG:
        SetWindowSubclass(GetDlgItem(h, IDC_LICENSE_EDIT), no_select_all, 1, 0);
        if (g_license_text)
            SetDlgItemTextW(h, IDC_LICENSE_EDIT, g_license_text);
        return TRUE;
    case WM_LICENSE_FOCUS: /* runs after the wizard has finished choosing where focus goes */
        SendDlgItemMessageW(h, IDC_LICENSE_EDIT, EM_SETSEL, 0, 0);
        SetFocus(GetDlgItem(h, IDC_ACCEPT));
        return TRUE;
    case WM_COMMAND:
        /* Windows selects all the text when a read-only edit box gets focus. Undo that
         * after the selection has been made, so the licence is never shown highlighted. */
        if (LOWORD(w) == IDC_LICENSE_EDIT && HIWORD(w) == EN_SETFOCUS)
            PostMessageW(GetDlgItem(h, IDC_LICENSE_EDIT), EM_SETSEL, 0, 0);
        if (LOWORD(w) == IDC_ACCEPT && HIWORD(w) == BN_CLICKED) {
            g_license_accepted = IsDlgButtonChecked(h, IDC_ACCEPT) == BST_CHECKED;
            buttons(h, PSWIZB_BACK | (g_license_accepted ? PSWIZB_NEXT : 0));
        }
        break;
    case WM_NOTIFY:
        if (((NMHDR *)l)->code == PSN_SETACTIVE) {
            buttons(h, PSWIZB_BACK | (g_license_accepted ? PSWIZB_NEXT : 0));
            SendDlgItemMessageW(h, IDC_LICENSE_EDIT, EM_SETSEL, 0, 0);
            SetFocus(GetDlgItem(h, IDC_ACCEPT)); /* start on the checkbox, not in the text */
            PostMessageW(h, WM_LICENSE_FOCUS, 0, 0);
            SetWindowLongPtrW(h, DWLP_MSGRESULT, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK DestinationProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_INITDIALOG:
        SetDlgItemTextW(h, IDC_DESTDIR, g_opt.dir);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(w) == IDC_BROWSE && HIWORD(w) == BN_CLICKED) {
            wchar_t cur[MAX_PATH], chosen[MAX_PATH];
            BROWSEINFOW bi;
            LPITEMIDLIST pidl;
            GetDlgItemTextW(h, IDC_DESTDIR, cur, MAX_PATH);
            ZeroMemory(&bi, sizeof bi);
            bi.hwndOwner = h;
            bi.lpszTitle = L"Choose the folder to install Railway Variable Auditor into:";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            bi.lpfn = browse_cb;
            bi.lParam = (LPARAM)cur;
            pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                if (SHGetPathFromIDListW(pidl, chosen))
                    SetDlgItemTextW(h, IDC_DESTDIR, chosen);
                CoTaskMemFree(pidl);
            }
        }
        break;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)l;
        if (nm->code == PSN_SETACTIVE) {
            buttons(h, PSWIZB_BACK | PSWIZB_NEXT);
            SetWindowLongPtrW(h, DWLP_MSGRESULT, 0);
            return TRUE;
        }
        if (nm->code == PSN_WIZNEXT) {
            GetDlgItemTextW(h, IDC_DESTDIR, g_opt.dir, MAX_PATH);
            if (!g_opt.dir[0]) {
                MessageBoxW(h, L"Please choose a folder to install into.", L"Railway Variable Auditor Setup", MB_OK | MB_ICONWARNING);
                SetWindowLongPtrW(h, DWLP_MSGRESULT, -1);
                return TRUE;
            }
        }
        break;
    }
    }
    return FALSE;
}

static INT_PTR CALLBACK OptionsProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)w;
    switch (m) {
    case WM_INITDIALOG:
        CheckDlgButton(h, IDC_OPT_MENU, g_opt.start_menu ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_OPT_DESKTOP, g_opt.desktop ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_OPT_PATH, g_opt.add_to_path ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)l;
        if (nm->code == PSN_SETACTIVE) {
            buttons(h, PSWIZB_BACK | PSWIZB_NEXT);
            SetWindowLongPtrW(h, DWLP_MSGRESULT, 0);
            return TRUE;
        }
        if (nm->code == PSN_WIZNEXT) {
            g_opt.start_menu = IsDlgButtonChecked(h, IDC_OPT_MENU) == BST_CHECKED;
            g_opt.desktop = IsDlgButtonChecked(h, IDC_OPT_DESKTOP) == BST_CHECKED;
            g_opt.add_to_path = IsDlgButtonChecked(h, IDC_OPT_PATH) == BST_CHECKED;
        }
        break;
    }
    }
    return FALSE;
}

static INT_PTR CALLBACK ReadyProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)w;
    if (m == WM_NOTIFY && ((NMHDR *)l)->code == PSN_SETACTIVE) {
        wchar_t text[MAX_PATH + 256];
        _snwprintf(text, MAX_PATH + 256,
                   L"Install folder:\r\n%ls\r\n\r\n"
                   L"Start menu folder: %ls\r\nDesktop shortcut: %ls\r\nAdd command line tool to PATH: %ls",
                   g_opt.dir, g_opt.start_menu ? L"yes" : L"no",
                   g_opt.desktop ? L"yes" : L"no", g_opt.add_to_path ? L"yes" : L"no");
        SetDlgItemTextW(h, IDC_READY_TEXT, text);
        SetDlgItemTextW(GetParent(h), ID_WIZNEXT, L"&Install");
        buttons(h, PSWIZB_BACK | PSWIZB_NEXT);
        SetWindowLongPtrW(h, DWLP_MSGRESULT, 0);
        return TRUE;
    }
    return FALSE;
}

typedef struct {
    HWND page;
} Job;

static void job_progress(int pct, void *ctx)
{
    PostMessageW(((Job *)ctx)->page, WM_INSTALL_PROGRESS, (WPARAM)pct, 0);
}

static DWORD WINAPI install_thread(LPVOID arg)
{
    Job *job = arg;
    Payload p;
    BOOL ok = payload_open(&p, TRUE, g_error, 512);
    if (ok) {
        ok = install_run(&p, &g_opt, job_progress, job, g_error, 512);
        payload_close(&p);
    }
    PostMessageW(job->page, WM_INSTALL_DONE, ok, 0);
    return 0;
}

static INT_PTR CALLBACK InstallingProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    static Job job;
    switch (m) {
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)l;
        if (nm->code == PSN_SETACTIVE) {
            buttons(h, 0);
            SendDlgItemMessageW(h, IDC_PROGRESS, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            if (!g_started) {
                g_started = g_installing = TRUE;
                job.page = h;
                SetDlgItemTextW(h, IDC_STATUS, L"Checking the installer...");
                CloseHandle(CreateThread(NULL, 0, install_thread, &job, 0, NULL));
            }
            SetWindowLongPtrW(h, DWLP_MSGRESULT, 0);
            return TRUE;
        }
        if (nm->code == PSN_QUERYCANCEL && g_installing) {
            SetWindowLongPtrW(h, DWLP_MSGRESULT, TRUE); /* do not cancel half way */
            return TRUE;
        }
        break;
    }
    case WM_INSTALL_PROGRESS:
        SendDlgItemMessageW(h, IDC_PROGRESS, PBM_SETPOS, w, 0);
        SetDlgItemTextW(h, IDC_STATUS, L"Copying files...");
        return TRUE;
    case WM_INSTALL_DONE:
        g_installing = FALSE;
        g_install_ok = (BOOL)w;
        if (g_install_ok) {
            SendDlgItemMessageW(h, IDC_PROGRESS, PBM_SETPOS, 100, 0);
            PropSheet_PressButton(GetParent(h), PSBTN_NEXT);
        } else {
            g_started = FALSE; /* allow a retry from Back */
            SetDlgItemTextW(h, IDC_STATUS, g_error);
            MessageBoxW(h, g_error, L"Railway Variable Auditor Setup", MB_OK | MB_ICONERROR);
            buttons(h, PSWIZB_BACK);
        }
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK FinishProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)w;
    if (m == WM_INITDIALOG) {
        CheckDlgButton(h, IDC_LAUNCH, BST_CHECKED);
        return TRUE;
    }
    if (m == WM_NOTIFY) {
        NMHDR *nm = (NMHDR *)l;
        if (nm->code == PSN_SETACTIVE) {
            wchar_t text[MAX_PATH + 256];
            _snwprintf(text, MAX_PATH + 256, L"Railway Variable Auditor was installed in:\r\n%ls\r\n\r\n%ls", g_opt.dir,
                       g_opt.add_to_path ? L"The command line tool was added to your PATH. Open a new terminal to use it."
                                         : L"");
            SetDlgItemTextW(h, IDC_FINISH_TEXT, text);
            buttons(h, PSWIZB_FINISH);
            SetWindowLongPtrW(h, DWLP_MSGRESULT, 0);
            return TRUE;
        }
        if (nm->code == PSN_WIZFINISH && IsDlgButtonChecked(h, IDC_LAUNCH) == BST_CHECKED) {
            wchar_t exe[MAX_PATH];
            _snwprintf(exe, MAX_PATH, L"%ls\\%ls", g_opt.dir, GUI_EXE);
            ShellExecuteW(NULL, L"open", exe, NULL, g_opt.dir, SW_SHOWNORMAL);
        }
    }
    return FALSE;
}

static PROPSHEETPAGEW make_page(int id, DLGPROC proc)
{
    PROPSHEETPAGEW psp;
    ZeroMemory(&psp, sizeof psp);
    psp.dwSize = sizeof psp;
    psp.dwFlags = PSP_DEFAULT;
    psp.hInstance = GetModuleHandleW(NULL);
    psp.pszTemplate = MAKEINTRESOURCEW(id);
    psp.pfnDlgProc = proc;
    return psp;
}

static int run_wizard(void)
{
    PROPSHEETPAGEW pages[7];
    PROPSHEETHEADERW psh;
    int n = 0;
    load_license();
    pages[n++] = make_page(IDD_WELCOME, WelcomeProc);
    if (g_license_text)
        pages[n++] = make_page(IDD_LICENSE, LicenseProc);
    pages[n++] = make_page(IDD_DESTINATION, DestinationProc);
    pages[n++] = make_page(IDD_OPTIONS, OptionsProc);
    pages[n++] = make_page(IDD_READY, ReadyProc);
    pages[n++] = make_page(IDD_INSTALLING, InstallingProc);
    pages[n++] = make_page(IDD_FINISH, FinishProc);
    ZeroMemory(&psh, sizeof psh);
    psh.dwSize = sizeof psh;
    psh.dwFlags = PSH_WIZARD | PSH_NOAPPLYNOW | PSH_PROPSHEETPAGE;
    psh.hInstance = GetModuleHandleW(NULL);
    psh.pszCaption = L"Railway Variable Auditor Setup";
    psh.nPages = (UINT)n;
    psh.ppsp = pages;
    PropertySheetW(&psh);
    return g_install_ok ? 0 : 1;
}

/* ---------- silent install and uninstall ---------- */

static int run_silent(void)
{
    Payload p;
    wchar_t err[512];
    BOOL ok = payload_open(&p, TRUE, err, 512) && install_run(&p, &g_opt, NULL, NULL, err, 512);
    payload_close(&p);
    if (!ok)
        MessageBoxW(NULL, err, L"Railway Variable Auditor Setup", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
    return ok ? 0 : 1;
}

static void self_dir(wchar_t *out)
{
    wchar_t *slash;
    GetModuleFileNameW(NULL, out, MAX_PATH);
    slash = wcsrchr(out, L'\\');
    if (slash)
        *slash = 0;
}

/* Stage 1 runs from the install folder, so it cannot delete itself. It copies itself to a
 * temporary file and starts that copy, which removes the install folder and then itself. */
static int uninstall_stage1(const wchar_t *dir, BOOL quiet)
{
    wchar_t self[MAX_PATH], tmp[MAX_PATH], tmpdir[MAX_PATH], cmd[MAX_PATH * 3];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (!quiet && MessageBoxW(NULL, L"Remove Railway Variable Auditor from this computer?", L"Railway Variable Auditor Uninstall",
                              MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 0;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    GetTempPathW(MAX_PATH, tmpdir);
    _snwprintf(tmp, MAX_PATH, L"%lsrva-uninstall-%lu.exe", tmpdir, GetCurrentProcessId());
    if (!CopyFileW(self, tmp, FALSE))
        return 1;
    _snwprintf(cmd, MAX_PATH * 3, L"\"%ls\" --uninstall-stage2 \"%ls\"%ls", tmp, dir, quiet ? L" --quiet" : L"");
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return 1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

static int uninstall_stage2(const wchar_t *dir, BOOL quiet)
{
    wchar_t err[512], un[MAX_PATH], self[MAX_PATH];
    BOOL ok;
    Sleep(800); /* let stage 1 exit so uninstall.exe can be deleted */
    ok = uninstall_run(dir, err, 512);
    _snwprintf(un, MAX_PATH, L"%ls\\%ls", dir, UNINSTALL_EXE);
    for (int i = 0; i < 20 && !DeleteFileW(un) && GetFileAttributesW(un) != INVALID_FILE_ATTRIBUTES; i++)
        Sleep(250);
    RemoveDirectoryW(dir);
    GetModuleFileNameW(NULL, self, MAX_PATH);
    MoveFileExW(self, NULL, MOVEFILE_DELAY_UNTIL_REBOOT); /* this temporary copy */
    if (!quiet)
        MessageBoxW(NULL, ok ? L"Railway Variable Auditor was removed." : err, L"Railway Variable Auditor Uninstall",
                    MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
    return ok ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, PWSTR cmdline, int show)
{
    int argc = 0, rc;
    LPWSTR *argv;
    BOOL uninstall = FALSE, stage2 = FALSE, quiet = FALSE, silent = FALSE;
    const wchar_t *dir_arg = NULL, *stage_dir = NULL;
    wchar_t existing[MAX_PATH], here[MAX_PATH];
    (void)hi;
    (void)hp;
    (void)cmdline;
    (void)show;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    {
        INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
        InitCommonControlsEx(&icc);
    }
    install_defaults(&g_opt);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        const wchar_t *a = argv[i];
        if (!wcscmp(a, L"--uninstall"))
            uninstall = TRUE;
        else if (!wcscmp(a, L"--uninstall-stage2") && i + 1 < argc) {
            stage2 = TRUE;
            stage_dir = argv[++i];
        } else if (!wcscmp(a, L"--quiet") || !wcscmp(a, L"--silent") || !_wcsicmp(a, L"/S") || !_wcsicmp(a, L"/quiet"))
            quiet = silent = TRUE;
        else if (!wcscmp(a, L"--dir") && i + 1 < argc)
            dir_arg = argv[++i];
        else if (!_wcsnicmp(a, L"/D=", 3))
            dir_arg = a + 3;
        else if (!wcscmp(a, L"--no-path"))
            g_opt.add_to_path = FALSE;
        else if (!wcscmp(a, L"--no-menu"))
            g_opt.start_menu = FALSE;
        else if (!wcscmp(a, L"--desktop"))
            g_opt.desktop = TRUE;
    }

    self_dir(here);
    if (stage2) {
        rc = uninstall_stage2(stage_dir, quiet);
    } else if (uninstall || wcsstr(GetCommandLineW(), L"uninstall.exe")) {
        /* From the install folder it removes that folder; otherwise it uses the registry. */
        wchar_t dir[MAX_PATH];
        if (dir_arg)
            wcsncpy(dir, dir_arg, MAX_PATH - 1);
        else if (_snwprintf(existing, MAX_PATH, L"%ls\\%ls", here, UNINSTALL_DAT) > 0
                 && GetFileAttributesW(existing) != INVALID_FILE_ATTRIBUTES)
            wcsncpy(dir, here, MAX_PATH - 1);
        else if (!install_find_existing(dir, MAX_PATH)) {
            MessageBoxW(NULL, L"Railway Variable Auditor is not installed for this user.", L"Railway Variable Auditor Uninstall", MB_OK | MB_ICONINFORMATION);
            return 1;
        }
        dir[MAX_PATH - 1] = 0;
        rc = uninstall_stage1(dir, quiet);
    } else {
        if (dir_arg) {
            wcsncpy(g_opt.dir, dir_arg, MAX_PATH - 1);
            g_opt.dir[MAX_PATH - 1] = 0;
        } else if (install_find_existing(existing, MAX_PATH)) {
            wcsncpy(g_opt.dir, existing, MAX_PATH - 1); /* upgrade in place */
        }
        rc = silent ? run_silent() : run_wizard();
    }
    if (argv)
        LocalFree(argv);
    CoUninitialize();
    return rc;
}
