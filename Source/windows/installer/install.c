/*
 * Copyright (c) 2026, Ross WD Cameron <ross@wexfordhouse.co.za>
 * Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
 */
#define COBJMACROS
#include "install.h"

#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "common.h"

static void set_err(wchar_t *err, DWORD len, const wchar_t *fmt, ...)
{
    va_list ap;
    if (!err || !len)
        return;
    va_start(ap, fmt);
    _vsnwprintf(err, len - 1, fmt, ap);
    va_end(ap);
    err[len - 1] = 0;
}

/* ---------- small helpers ---------- */

static wchar_t *utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void join(wchar_t *out, size_t len, const wchar_t *a, const wchar_t *b)
{
    _snwprintf(out, len, L"%ls\\%ls", a, b);
    out[len - 1] = 0;
}

static BOOL make_dirs(const wchar_t *path)
{
    wchar_t tmp[MAX_PATH];
    wcsncpy(tmp, path, MAX_PATH - 1);
    tmp[MAX_PATH - 1] = 0;
    for (wchar_t *p = tmp + 3; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    return (CreateDirectoryW(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
}

void install_defaults(InstallOptions *o)
{
    wchar_t local[MAX_PATH];
    memset(o, 0, sizeof *o);
    o->add_to_path = TRUE;
    o->start_menu = TRUE;
    o->desktop = FALSE;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
        _snwprintf(o->dir, MAX_PATH, L"%ls\\Programs\\%ls", local, APP_NAME);
}

/* ---------- PATH (HKCU\Environment) ---------- */

static wchar_t *read_user_path(HKEY key)
{
    DWORD type = 0, size = 0;
    wchar_t *buf;
    LONG rc = RegQueryValueExW(key, L"Path", NULL, &type, NULL, &size);
    if (rc == ERROR_FILE_NOT_FOUND)
        return calloc(1, sizeof(wchar_t));
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return NULL;
    buf = malloc(size + sizeof(wchar_t));
    if (!buf)
        return NULL;
    if (RegQueryValueExW(key, L"Path", NULL, &type, (BYTE *)buf, &size) != ERROR_SUCCESS) {
        free(buf);
        return NULL;
    }
    buf[size / sizeof(wchar_t)] = 0;
    return buf;
}

static BOOL path_has(const wchar_t *list, const wchar_t *dir)
{
    size_t n = wcslen(dir);
    for (const wchar_t *p = list; *p;) {
        const wchar_t *sep = wcschr(p, L';');
        size_t seg = sep ? (size_t)(sep - p) : wcslen(p);
        if (seg == n && _wcsnicmp(p, dir, n) == 0)
            return TRUE;
        if (!sep)
            break;
        p = sep + 1;
    }
    return FALSE;
}

static wchar_t *path_without(const wchar_t *list, const wchar_t *dir)
{
    size_t n = wcslen(dir);
    wchar_t *out = calloc(wcslen(list) + 1, sizeof(wchar_t));
    BOOL first = TRUE;
    if (!out)
        return NULL;
    for (const wchar_t *p = list; *p;) {
        const wchar_t *sep = wcschr(p, L';');
        size_t seg = sep ? (size_t)(sep - p) : wcslen(p);
        if (seg && !(seg == n && _wcsnicmp(p, dir, n) == 0)) {
            if (!first)
                wcscat(out, L";");
            wcsncat(out, p, seg);
            first = FALSE;
        }
        if (!sep)
            break;
        p = sep + 1;
    }
    return out;
}

static void broadcast_environment(void)
{
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 3000, &r);
}

static void bin_of(const wchar_t *dir, wchar_t *out)
{
    _snwprintf(out, MAX_PATH, L"%ls\\%ls", dir, BIN_DIR);
    out[MAX_PATH - 1] = 0;
}

BOOL install_dir_on_path(const wchar_t *dir_in)
{
    wchar_t dir[MAX_PATH];
    HKEY key;
    bin_of(dir_in, dir);
    wchar_t *cur;
    BOOL has = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;
    cur = read_user_path(key);
    RegCloseKey(key);
    if (cur) {
        has = path_has(cur, dir);
        free(cur);
    }
    return has;
}

static BOOL path_add(const wchar_t *dir_in)
{
    wchar_t dir[MAX_PATH];
    HKEY key;
    wchar_t *cur, *next;
    size_t len;
    BOOL ok;
    bin_of(dir_in, dir);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;
    cur = read_user_path(key);
    if (!cur) {
        RegCloseKey(key);
        return FALSE;
    }
    if (path_has(cur, dir)) {
        free(cur);
        RegCloseKey(key);
        return TRUE;
    }
    len = wcslen(cur) + wcslen(dir) + 2;
    next = malloc(len * sizeof(wchar_t));
    if (!next) {
        free(cur);
        RegCloseKey(key);
        return FALSE;
    }
    if (*cur)
        _snwprintf(next, len, L"%ls;%ls", cur, dir);
    else
        wcscpy(next, dir);
    ok = RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ, (const BYTE *)next, (DWORD)((wcslen(next) + 1) * sizeof(wchar_t)))
         == ERROR_SUCCESS;
    free(cur);
    free(next);
    RegCloseKey(key);
    broadcast_environment();
    return ok;
}

static void path_remove(const wchar_t *dir_in)
{
    wchar_t dir[MAX_PATH];
    HKEY key;
    wchar_t *cur, *next;
    bin_of(dir_in, dir);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    cur = read_user_path(key);
    if (cur && path_has(cur, dir) && (next = path_without(cur, dir))) {
        RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ, (const BYTE *)next, (DWORD)((wcslen(next) + 1) * sizeof(wchar_t)));
        free(next);
    }
    free(cur);
    RegCloseKey(key);
    broadcast_environment();
}

/* ---------- registry: Add/Remove Programs ---------- */

static void set_str(HKEY root, const wchar_t *subkey, const wchar_t *name, const wchar_t *value)
{
    HKEY key;
    if (RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

static void set_dword(HKEY root, const wchar_t *subkey, const wchar_t *name, DWORD value)
{
    HKEY key;
    if (RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof value);
        RegCloseKey(key);
    }
}

static BOOL get_str(HKEY root, const wchar_t *subkey, const wchar_t *name, wchar_t *out, DWORD len)
{
    HKEY key;
    DWORD size = len * sizeof(wchar_t), type;
    BOOL ok = FALSE;
    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        ok = RegQueryValueExW(key, name, NULL, &type, (BYTE *)out, &size) == ERROR_SUCCESS && type == REG_SZ;
        RegCloseKey(key);
    }
    return ok;
}

static void register_uninstall(const wchar_t *dir, DWORD size_kb)
{
    wchar_t un[MAX_PATH], cmd[MAX_PATH + 32], quiet[MAX_PATH + 48], icon[MAX_PATH + 8], gui[MAX_PATH];
    join(un, MAX_PATH, dir, UNINSTALL_EXE);
    join(gui, MAX_PATH, dir, GUI_EXE);
    _snwprintf(cmd, MAX_PATH + 32, L"\"%ls\" --uninstall", un);
    _snwprintf(quiet, MAX_PATH + 48, L"\"%ls\" --uninstall --quiet", un);
    _snwprintf(icon, MAX_PATH + 8, L"%ls,0", gui);
    set_str(HKEY_CURRENT_USER, UNINSTALL_KEY, L"DisplayName", APP_NAME);
    set_str(HKEY_CURRENT_USER, UNINSTALL_KEY, L"DisplayVersion", APP_VERSION);
    set_str(HKEY_CURRENT_USER, UNINSTALL_KEY, L"Publisher", APP_PUBLISHER);
    set_str(HKEY_CURRENT_USER, UNINSTALL_KEY, L"InstallLocation", dir);
    set_str(HKEY_CURRENT_USER, UNINSTALL_KEY, L"DisplayIcon", icon);
    set_str(HKEY_CURRENT_USER, UNINSTALL_KEY, L"UninstallString", cmd);
    set_str(HKEY_CURRENT_USER, UNINSTALL_KEY, L"QuietUninstallString", quiet);
    set_dword(HKEY_CURRENT_USER, UNINSTALL_KEY, L"EstimatedSize", size_kb);
    set_dword(HKEY_CURRENT_USER, UNINSTALL_KEY, L"NoModify", 1);
    set_dword(HKEY_CURRENT_USER, UNINSTALL_KEY, L"NoRepair", 1);
}

BOOL install_find_existing(wchar_t *dir, DWORD len)
{
    return get_str(HKEY_CURRENT_USER, UNINSTALL_KEY, L"InstallLocation", dir, len) && dir[0];
}

/* ---------- shortcuts ---------- */

static BOOL make_link(const wchar_t *lnk, const wchar_t *target, const wchar_t *args, const wchar_t *workdir,
                      const wchar_t *icon, const wchar_t *desc)
{
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    BOOL ok = FALSE;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl)))
        return FALSE;
    IShellLinkW_SetPath(sl, target);
    if (args)
        IShellLinkW_SetArguments(sl, args);
    IShellLinkW_SetWorkingDirectory(sl, workdir);
    IShellLinkW_SetIconLocation(sl, icon, 0);
    IShellLinkW_SetDescription(sl, desc);
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        ok = SUCCEEDED(IPersistFile_Save(pf, lnk, TRUE));
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
    return ok;
}

static BOOL known_folder(int csidl, wchar_t *out)
{
    return SHGetFolderPathW(NULL, csidl | CSIDL_FLAG_CREATE, NULL, 0, out) == S_OK;
}

static void create_shortcuts(const wchar_t *dir, BOOL start_menu, BOOL desktop)
{
    wchar_t gui[MAX_PATH], un[MAX_PATH], base[MAX_PATH], folder[MAX_PATH], lnk[MAX_PATH], docs[MAX_PATH];
    join(gui, MAX_PATH, dir, GUI_EXE);
    join(un, MAX_PATH, dir, UNINSTALL_EXE);
    join(docs, MAX_PATH, dir, L"Documentation\\Website\\index.html");
    if (start_menu && known_folder(CSIDL_PROGRAMS, base)) {
        join(folder, MAX_PATH, base, APP_NAME);
        CreateDirectoryW(folder, NULL);
        join(lnk, MAX_PATH, folder, L"Railway Variable Auditor.lnk");
        make_link(lnk, gui, NULL, dir, gui, L"Write every Railway variable to one CSV file");
        join(lnk, MAX_PATH, folder, L"Railway Variable Auditor Documentation.lnk");
        make_link(lnk, docs, NULL, dir, gui, L"Railway Variable Auditor documentation");
        join(lnk, MAX_PATH, folder, L"Uninstall Railway Variable Auditor.lnk");
        make_link(lnk, un, L"--uninstall", dir, gui, L"Remove Railway Variable Auditor");
    }
    if (desktop && known_folder(CSIDL_DESKTOPDIRECTORY, base)) {
        join(lnk, MAX_PATH, base, L"Railway Variable Auditor.lnk");
        make_link(lnk, gui, NULL, dir, gui, L"Write every Railway variable to one CSV file");
    }
}

static void remove_shortcuts(void)
{
    wchar_t base[MAX_PATH], folder[MAX_PATH], lnk[MAX_PATH];
    if (known_folder(CSIDL_PROGRAMS, base)) {
        join(folder, MAX_PATH, base, APP_NAME);
        join(lnk, MAX_PATH, folder, L"Railway Variable Auditor.lnk");
        DeleteFileW(lnk);
        join(lnk, MAX_PATH, folder, L"Railway Variable Auditor Documentation.lnk");
        DeleteFileW(lnk);
        join(lnk, MAX_PATH, folder, L"Uninstall Railway Variable Auditor.lnk");
        DeleteFileW(lnk);
        RemoveDirectoryW(folder);
    }
    if (known_folder(CSIDL_DESKTOPDIRECTORY, base)) {
        join(lnk, MAX_PATH, base, L"Railway Variable Auditor.lnk");
        DeleteFileW(lnk);
    }
}

/* ---------- extracting the payload ---------- */

typedef struct {
    const wchar_t *dir;
    FILE *list;          /* uninstall.dat, UTF-16 */
    uint64_t done;
    uint64_t total;
    progress_fn progress;
    void *ctx;
    wchar_t *err;
    DWORD err_len;
    int last_percent;
} Extract;

static BOOL write_entry(const char *path, const BYTE *data, uint64_t size, void *vctx)
{
    Extract *x = vctx;
    wchar_t *rel = utf8_to_wide(path), full[MAX_PATH], parent[MAX_PATH];
    HANDLE h;
    DWORD w = 0;
    BOOL ok;

    if (!rel)
        return FALSE;
    for (wchar_t *c = rel; *c; c++)
        if (*c == L'/')
            *c = L'\\';
    join(full, MAX_PATH, x->dir, rel);
    wcscpy(parent, full);
    *wcsrchr(parent, L'\\') = 0;
    make_dirs(parent);
    h = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        set_err(x->err, x->err_len, L"Could not write '%ls' (error %lu). If Railway Variable Auditor is open, close it and try again.",
                full, GetLastError());
        free(rel);
        return FALSE;
    }
    ok = TRUE;
    for (uint64_t off = 0; ok && off < size;) {
        DWORD n = (size - off) > (1u << 24) ? (1u << 24) : (DWORD)(size - off);
        ok = WriteFile(h, data + off, n, &w, NULL) && w == n;
        off += n;
    }
    CloseHandle(h);
    if (!ok) {
        set_err(x->err, x->err_len, L"Could not write '%ls' (error %lu).", full, GetLastError());
        free(rel);
        return FALSE;
    }
    fwprintf(x->list, L"%ls\n", rel);
    free(rel);
    x->done += size;
    if (x->total) {
        int pct = (int)(x->done * 95 / x->total);
        if (pct != x->last_percent) {
            x->last_percent = pct;
            x->progress(pct, x->ctx);
        }
    }
    return TRUE;
}

static void nop_progress(int percent, void *ctx)
{
    (void)percent;
    (void)ctx;
}

BOOL install_run(Payload *p, const InstallOptions *o, progress_fn progress, void *ctx, wchar_t *err, DWORD err_len)
{
    Extract x;
    wchar_t list_path[MAX_PATH], un[MAX_PATH];
    BOOL ok;

    if (!make_dirs(o->dir)) {
        set_err(err, err_len, L"Could not create '%ls' (error %lu).", o->dir, GetLastError());
        return FALSE;
    }
    join(list_path, MAX_PATH, o->dir, UNINSTALL_DAT);
    memset(&x, 0, sizeof x);
    x.dir = o->dir;
    x.list = _wfopen(list_path, L"w, ccs=UTF-16LE");
    x.total = p->total_size;
    x.progress = progress ? progress : nop_progress;
    x.ctx = ctx;
    x.err = err;
    x.err_len = err_len;
    x.last_percent = -1;
    if (!x.list) {
        set_err(err, err_len, L"Could not write the uninstall list in '%ls'.", o->dir);
        return FALSE;
    }
    fwprintf(x.list, L"%ls\n", UNINSTALL_EXE);
    ok = payload_walk(p, TRUE, write_entry, &x, err, err_len);
    if (ok) {
        join(un, MAX_PATH, o->dir, UNINSTALL_EXE);
        ok = payload_write_stub(p, un, err, err_len);
    }
    if (ok) {
        fwprintf(x.list, L"#path=%d\n", o->add_to_path);
    }
    fclose(x.list);
    if (!ok)
        return FALSE;

    register_uninstall(o->dir, (DWORD)(p->total_size / 1024));
    if (o->add_to_path)
        path_add(o->dir);
    create_shortcuts(o->dir, o->start_menu, o->desktop);
    x.progress(100, x.ctx);
    return TRUE;
}

/* ---------- uninstall ---------- */

static int by_length_desc(const void *a, const void *b)
{
    return (int)(wcslen(*(wchar_t *const *)b) - wcslen(*(wchar_t *const *)a));
}

BOOL uninstall_run(const wchar_t *dir, wchar_t *err, DWORD err_len)
{
    wchar_t list_path[MAX_PATH], line[MAX_PATH], full[MAX_PATH];
    wchar_t **dirs = NULL;
    size_t ndirs = 0, cap = 0;
    FILE *f;

    join(list_path, MAX_PATH, dir, UNINSTALL_DAT);
    f = _wfopen(list_path, L"r, ccs=UTF-16LE");
    if (!f) {
        set_err(err, err_len, L"Cannot find the uninstall list in '%ls'.", dir);
        return FALSE;
    }
    while (fgetws(line, MAX_PATH, f)) {
        size_t n = wcslen(line);
        wchar_t *slash;
        while (n && (line[n - 1] == L'\n' || line[n - 1] == L'\r'))
            line[--n] = 0;
        if (!n || line[0] == L'#' || wcsstr(line, L".."))
            continue;
        if (_wcsicmp(line, UNINSTALL_EXE) == 0)
            continue; /* removed last, by the copy that is running */
        join(full, MAX_PATH, dir, line);
        SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(full);
        for (slash = wcsrchr(line, L'\\'); slash; slash = wcsrchr(line, L'\\')) {
            *slash = 0;
            if (ndirs == cap) {
                cap = cap ? cap * 2 : 64;
                dirs = realloc(dirs, cap * sizeof *dirs);
            }
            dirs[ndirs++] = _wcsdup(line);
        }
    }
    fclose(f);
    qsort(dirs, ndirs, sizeof *dirs, by_length_desc);
    for (size_t i = 0; i < ndirs; i++) {
        join(full, MAX_PATH, dir, dirs[i]);
        RemoveDirectoryW(full);
        free(dirs[i]);
    }
    free(dirs);
    DeleteFileW(list_path);

    remove_shortcuts();
    path_remove(dir);
    RegDeleteTreeW(HKEY_CURRENT_USER, UNINSTALL_KEY);
    return TRUE;
}
