
/*
 *  File Transfer Client  -  Win32 GUI
 *  Talks to server.c over TCP on port 8080
 *
 *  Controls
 *  --------
 *  [Connect]          connect to server (IP + port from edit boxes)
 *  [Refresh List]     ask server for its file list  (LIST command)
 *  [Upload File]      browse and upload a file
 *  [Download Selected] download the file selected in the list
 *  [Disconnect]       send QUIT and close socket
 *
 *  Compile (MSYS2 ucrt64):
 *    gcc gui_client.c -o gui_client.exe ^
 *        -lws2_32 -lcomdlg32 -lcomctl32 -mwindows
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <direct.h>

/* ── window / control IDs ─────────────────────────────────────────────── */
#define IDC_EDIT_IP         101
#define IDC_EDIT_PORT       102
#define IDC_BTN_CONNECT     103
#define IDC_BTN_REFRESH     104
#define IDC_BTN_UPLOAD      105
#define IDC_BTN_DOWNLOAD    106
#define IDC_BTN_DISCONNECT  107
#define IDC_LIST_FILES      108
#define IDC_PROGRESS        109
#define IDC_STATUS          110
#define IDC_LOG             111

#define PORT_DEFAULT  9000
#define BUFFER_SIZE   8192
#define CLIENT_DIR    "client_files"
#define MAX_LINE      1024

/* posted from worker threads */
#define WM_TRANSFER_DONE  (WM_USER + 1)
#define WM_TRANSFER_PROG  (WM_USER + 2)
#define WM_LOG_LINE       (WM_USER + 3)
#define WM_LIST_READY     (WM_USER + 4)

/* ── globals ──────────────────────────────────────────────────────────── */
static HWND g_hwnd;
static HWND g_edit_ip, g_edit_port;
static HWND g_btn_connect, g_btn_refresh, g_btn_upload;
static HWND g_btn_download, g_btn_disconnect;
static HWND g_list_files, g_progress, g_status_bar, g_log;

static SOCKET g_sock   = INVALID_SOCKET;
static HFONT  g_font_ui, g_font_title, g_font_mono;
static HBRUSH g_brush_bg, g_brush_panel;

#define CLR_BG     RGB(245, 247, 250)
#define CLR_PANEL  RGB(255, 255, 255)
#define CLR_TEXT   RGB(30,  30,  30)

/* ── logging ──────────────────────────────────────────────────────────── */
static void log_msg(const char *text)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[600];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d]  %s",
             st.wHour, st.wMinute, st.wSecond, text);

    int len = GetWindowTextLengthA(g_log);
    SendMessageA(g_log, EM_SETSEL, len, len);
    SendMessageA(g_log, EM_REPLACESEL, 0, (LPARAM)line);
    SendMessageA(g_log, EM_REPLACESEL, 0, (LPARAM)"\r\n");
    SendMessageA(g_log, EM_SCROLLCARET, 0, 0);
}

static void set_status(const char *text)
{
    SendMessageA(g_status_bar, SB_SETTEXTA, 0, (LPARAM)text);
}

/* ── socket helpers ───────────────────────────────────────────────────── */
static int net_send_all(const char *data, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(g_sock, data + sent, len - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

static int net_recv_all(char *buf, int len)
{
    int done = 0;
    while (done < len) {
        int n = recv(g_sock, buf + done, len - done, 0);
        if (n <= 0) return 0;
        done += n;
    }
    return 1;
}

static int net_recv_line(char *line, int cap)
{
    int  i = 0;
    char ch;
    while (i < cap - 1) {
        if (recv(g_sock, &ch, 1, 0) <= 0) return 0;
        if (ch == '\n') break;
        if (ch != '\r') line[i++] = ch;
    }
    line[i] = '\0';
    return 1;
}

/* ── enable / disable the whole UI ───────────────────────────────────── */
static void ui_set_connected(int yes)
{
    EnableWindow(g_btn_connect,    !yes);
    EnableWindow(g_edit_ip,        !yes);
    EnableWindow(g_edit_port,      !yes);
    EnableWindow(g_btn_refresh,     yes);
    EnableWindow(g_btn_upload,      yes);
    EnableWindow(g_btn_download,    yes);
    EnableWindow(g_btn_disconnect,  yes);
}

/* ── transfer arg passed to worker threads ──────────────────────────── */
typedef struct { char path[MAX_PATH]; } TransferArg;

/* ── UPLOAD worker ───────────────────────────────────────────────────── */
static DWORD WINAPI upload_thread(LPVOID arg)
{
    TransferArg *ta = (TransferArg *)arg;

    FILE *fp = fopen(ta->path, "rb");
    if (!fp) {
        PostMessageA(g_hwnd, WM_LOG_LINE, 0, (LPARAM)_strdup("Upload failed: cannot open file."));
        PostMessageA(g_hwnd, WM_TRANSFER_DONE, 0, 0);
        free(ta); return 1;
    }

    _fseeki64(fp, 0, SEEK_END);
    long long fsize = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);

    const char *base = strrchr(ta->path, '\\');
    base = base ? base + 1 : ta->path;

    char header[MAX_LINE * 3];
    int  hlen = snprintf(header, sizeof(header),
                         "UPLOAD\n%s\n%llu\n", base, (unsigned long long)fsize);

    if (!net_send_all(header, hlen)) {
        fclose(fp);
        PostMessageA(g_hwnd, WM_LOG_LINE, 0, (LPARAM)_strdup("Upload failed: connection error."));
        PostMessageA(g_hwnd, WM_TRANSFER_DONE, 0, 0);
        free(ta); return 1;
    }

    char buf[BUFFER_SIZE];
    unsigned long long remaining = (unsigned long long)fsize;
    unsigned long long sent_bytes = 0;

    while (remaining > 0) {
        size_t chunk = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : (size_t)remaining;
        size_t n = fread(buf, 1, chunk, fp);
        if (n == 0 || !net_send_all(buf, (int)n)) {
            fclose(fp);
            PostMessageA(g_hwnd, WM_LOG_LINE, 0, (LPARAM)_strdup("Upload aborted."));
            PostMessageA(g_hwnd, WM_TRANSFER_DONE, 0, 0);
            free(ta); return 1;
        }
        sent_bytes += n;
        remaining  -= n;
        int prog = (fsize > 0) ? (int)((sent_bytes * 1000) / (unsigned long long)fsize) : 1000;
        PostMessageA(g_hwnd, WM_TRANSFER_PROG, (WPARAM)prog, 1000);
    }
    fclose(fp);

    char resp[MAX_LINE];
    if (!net_recv_line(resp, sizeof(resp))) {
        PostMessageA(g_hwnd, WM_LOG_LINE, 0, (LPARAM)_strdup("No server response after upload."));
    } else if (strcmp(resp, "OK") == 0) {
        char *msg = (char *)malloc(256);
        snprintf(msg, 256, "Upload OK: %s  (%lld bytes)", base, fsize);
        PostMessageA(g_hwnd, WM_LOG_LINE, 0, (LPARAM)msg);
    } else {
        char *msg = (char *)malloc(256);
        snprintf(msg, 256, "Server replied: %s", resp);
        PostMessageA(g_hwnd, WM_LOG_LINE, 0, (LPARAM)msg);
    }

    PostMessageA(g_hwnd, WM_TRANSFER_DONE, 0, 0);
    free(ta); return 0;
}

/* ── DOWNLOAD worker ─────────────────────────────────────────────────── */
static DWORD WINAPI download_thread(LPVOID arg)
{
    TransferArg *ta = (TransferArg *)arg;

    char header[MAX_LINE * 2];
    int  hlen = snprintf(header, sizeof(header), "DOWNLOAD\n%s\n", ta->path);
    if (!net_send_all(header, hlen)) {
        PostMessageA(g_hwnd, WM_LOG_LINE, 0, (LPARAM)_strdup("Download failed: send error."));
        PostMessageA(g_hwnd, WM_TRANSFER_DONE, 0, 0);
        free(ta); return 1;
    }

    char resp[MAX_LINE];
    if (!net_recv_line(resp, sizeof(resp))) {
        PostMessageA(g_hwnd, WM_LOG_LINE, 0, (LPARAM)_strdup("Download failed: no response."));
        PostMessageA(g_hwnd, WM_TRANSFER_DONE, 0, 0);
        free(ta); return 1;
    }

    if (strncmp(resp, "OK ", 3) != 0) {
        char *msg = (char *)malloc(256);
        snprintf(msg, 256, "Server replied: %s", resp);
        PostMessageA(g_hwnd, WM_LOG_LINE, 0, (LPARAM)msg);
        PostMessageA(g_hwnd, WM_TRANSFER_DONE, 0, 0);
        free(ta); return 1;
    }

    unsigned long long fsize = 0;
    sscanf(resp + 3, "%llu", &fsize);

    _mkdir(CLIENT_DIR);
    char out_path[MAX_PATH];
    snprintf(out_path, sizeof(out_path), "%s\\%s", CLIENT_DIR, ta->path);

    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        PostMessageA(g_hwnd, WM_LOG_LINE, 0, (LPARAM)_strdup("Cannot create local file."));
        PostMessageA(g_hwnd, WM_TRANSFER_DONE, 0, 0);
        free(ta); return 1;
    }

    char buf[BUFFER_SIZE];
    unsigned long long remaining  = fsize;
    unsigned long long recv_bytes = 0;

    while (remaining > 0) {
        int chunk = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : (int)remaining;
        if (!net_recv_all(buf, chunk)) {
            fclose(fp); remove(out_path);
            PostMessageA(g_hwnd, WM_LOG_LINE, 0, (LPARAM)_strdup("Download interrupted."));
            PostMessageA(g_hwnd, WM_TRANSFER_DONE, 0, 0);
            free(ta); return 1;
        }
        fwrite(buf, 1, chunk, fp);
        recv_bytes += chunk;
        remaining  -= chunk;
        int prog = (fsize > 0) ? (int)((recv_bytes * 1000) / fsize) : 1000;
        PostMessageA(g_hwnd, WM_TRANSFER_PROG, (WPARAM)prog, 1000);
    }
    fclose(fp);

    char *msg = (char *)malloc(512);
    snprintf(msg, 512, "Download OK: %s  (%llu bytes)  -> %s", ta->path, fsize, out_path);
    PostMessageA(g_hwnd, WM_LOG_LINE, 0, (LPARAM)msg);
    PostMessageA(g_hwnd, WM_TRANSFER_DONE, 0, 0);
    free(ta); return 0;
}

/* ── LIST worker ─────────────────────────────────────────────────────── */
static DWORD WINAPI list_thread(LPVOID arg)
{
    (void)arg;
    if (!net_send_all("LIST\n", 5)) {
        PostMessageA(g_hwnd, WM_LOG_LINE, 0, (LPARAM)_strdup("LIST failed."));
        PostMessageA(g_hwnd, WM_TRANSFER_DONE, 0, 0);
        return 1;
    }

    char *all = (char *)calloc(1, 131072);
    if (!all) { PostMessageA(g_hwnd, WM_TRANSFER_DONE, 0, 0); return 1; }

    char line[MAX_LINE];
    while (net_recv_line(line, sizeof(line))) {
        if (line[0] == '\0') break;
        strncat(all, line,  131071 - strlen(all));
        strncat(all, "\n",  131071 - strlen(all));
    }

    PostMessageA(g_hwnd, WM_LIST_READY,    0, (LPARAM)all);
    PostMessageA(g_hwnd, WM_TRANSFER_DONE, 0, 0);
    return 0;
}

/* ── button handlers ─────────────────────────────────────────────────── */
static void do_connect(void)
{
    char ip[128] = "", port_str[16] = "";
    GetWindowTextA(g_edit_ip,   ip,       sizeof(ip));
    GetWindowTextA(g_edit_port, port_str, sizeof(port_str));
    if (ip[0] == '\0') strcpy(ip, "127.0.0.1");
    int port = atoi(port_str);
    if (port <= 0 || port > 65535) port = PORT_DEFAULT;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_msg("Winsock init failed."); return;
    }

    g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_sock == INVALID_SOCKET) {
        log_msg("Socket creation failed."); WSACleanup(); return;
    }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons((u_short)port);

    unsigned long in_addr = inet_addr(ip);
    if (in_addr == INADDR_NONE && strcmp(ip, "255.255.255.255") != 0) {
        struct hostent *he = gethostbyname(ip);
        if (!he || !he->h_addr_list[0]) {
            log_msg("Invalid Server IP address or hostname.");
            closesocket(g_sock); g_sock = INVALID_SOCKET;
            WSACleanup();
            set_status("Not connected."); return;
        }
        memcpy(&srv.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
    } else {
        srv.sin_addr.s_addr = in_addr;
    }

    set_status("Connecting...");
    if (connect(g_sock, (struct sockaddr *)&srv, sizeof(srv)) == SOCKET_ERROR) {
        char msg[200];
        snprintf(msg, sizeof(msg), "Cannot connect to %s:%d  (err %d)",
                 ip, port, WSAGetLastError());
        log_msg(msg);
        closesocket(g_sock); g_sock = INVALID_SOCKET;
        WSACleanup();
        set_status("Not connected."); return;
    }

    char msg[200];
    snprintf(msg, sizeof(msg), "Connected to %s:%d", ip, port);
    log_msg(msg);
    set_status(msg);
    ui_set_connected(1);
    SendMessage(g_progress, PBM_SETPOS, 0, 0);
}

static void do_disconnect(void)
{
    if (g_sock == INVALID_SOCKET) return;
    net_send_all("QUIT\n", 5);
    closesocket(g_sock); g_sock = INVALID_SOCKET;
    WSACleanup();
    SendMessageA(g_list_files, LB_RESETCONTENT, 0, 0);
    SendMessage(g_progress, PBM_SETPOS, 0, 0);
    log_msg("Disconnected.");
    set_status("Not connected.");
    ui_set_connected(0);
}

static void do_refresh(void)
{
    if (g_sock == INVALID_SOCKET) return;
    log_msg("Refreshing file list...");
    ui_set_connected(0);
    EnableWindow(g_btn_disconnect, 1);
    HANDLE t = CreateThread(NULL, 0, list_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static void do_upload(void)
{
    if (g_sock == INVALID_SOCKET) return;
    OPENFILENAMEA ofn;
    char path[MAX_PATH] = "";
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = g_hwnd;
    ofn.lpstrFile    = path;
    ofn.nMaxFile     = sizeof(path);
    ofn.lpstrFilter  = "All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return;

    char logmsg[MAX_PATH + 20];
    snprintf(logmsg, sizeof(logmsg), "Uploading: %s", path);
    log_msg(logmsg);
    set_status("Uploading...");

    TransferArg *ta = (TransferArg *)malloc(sizeof(TransferArg));
    if (!ta) return;
    strncpy(ta->path, path, MAX_PATH - 1);

    ui_set_connected(0);
    EnableWindow(g_btn_disconnect, 1);
    SendMessage(g_progress, PBM_SETPOS, 0, 0);

    HANDLE t = CreateThread(NULL, 0, upload_thread, ta, 0, NULL);
    if (t) CloseHandle(t);
}

static void do_download(void)
{
    if (g_sock == INVALID_SOCKET) return;
    int idx = (int)SendMessageA(g_list_files, LB_GETCURSEL, 0, 0);
    if (idx == LB_ERR) {
        log_msg("Select a file from the list first."); return;
    }

    char entry[MAX_PATH + 80];
    SendMessageA(g_list_files, LB_GETTEXT, idx, (LPARAM)entry);

    /* entry is "filename   size unit" – grab just the filename */
    char filename[MAX_PATH];
    sscanf(entry, "%s", filename);

    char logmsg[MAX_PATH + 20];
    snprintf(logmsg, sizeof(logmsg), "Downloading: %s", filename);
    log_msg(logmsg);
    set_status("Downloading...");

    TransferArg *ta = (TransferArg *)malloc(sizeof(TransferArg));
    if (!ta) return;
    strncpy(ta->path, filename, MAX_PATH - 1);

    ui_set_connected(0);
    EnableWindow(g_btn_disconnect, 1);
    SendMessage(g_progress, PBM_SETPOS, 0, 0);

    HANDLE t = CreateThread(NULL, 0, download_thread, ta, 0, NULL);
    if (t) CloseHandle(t);
}

/* ── helper: create a child control with a font ─────────────────────── */
static HWND mk_label(HWND p, const char *txt, int x, int y, int w, int h, HFONT f)
{
    HWND lbl = CreateWindowExA(0, "STATIC", txt,
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                x, y, w, h, p, NULL,
                                GetModuleHandleA(NULL), NULL);
    SendMessageA(lbl, WM_SETFONT, (WPARAM)f, TRUE);
    return lbl;
}

static HWND mk_edit(HWND p, const char *txt, int x, int y, int w, int h,
                     DWORD style, int id, HFONT f)
{
    HWND ed = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", txt,
                               WS_CHILD | WS_VISIBLE | style,
                               x, y, w, h, p, (HMENU)(intptr_t)id,
                               GetModuleHandleA(NULL), NULL);
    SendMessageA(ed, WM_SETFONT, (WPARAM)f, TRUE);
    return ed;
}

static HWND mk_button(HWND p, const char *txt, int x, int y, int w, int h,
                       int id, HFONT f)
{
    HWND btn = CreateWindowExA(0, "BUTTON", txt,
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                x, y, w, h, p, (HMENU)(intptr_t)id,
                                GetModuleHandleA(NULL), NULL);
    SendMessageA(btn, WM_SETFONT, (WPARAM)f, TRUE);
    return btn;
}

static HFONT mk_font(int sz, int bold, const char *name)
{
    return CreateFontA(sz, 0, 0, 0,
                       bold ? FW_BOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, name);
}

/* ── WndProc ─────────────────────────────────────────────────────────── */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_brush_bg    = CreateSolidBrush(CLR_BG);
        g_brush_panel = CreateSolidBrush(CLR_PANEL);

        g_font_title = mk_font(-17, 1, "Segoe UI");
        g_font_ui    = mk_font(-14, 0, "Segoe UI");
        g_font_mono  = mk_font(-13, 0, "Consolas");

        /* title */
        mk_label(hwnd, "TCP File Transfer Client", 18, 12, 400, 28, g_font_title);

        /* connection row */
        mk_label(hwnd, "Server IP:", 18, 56, 76, 20, g_font_ui);
        g_edit_ip   = mk_edit(hwnd, "127.0.0.1",
                               97, 53, 156, 24, ES_AUTOHSCROLL, IDC_EDIT_IP, g_font_ui);
        mk_label(hwnd, "Port:", 262, 56, 36, 20, g_font_ui);
        g_edit_port = mk_edit(hwnd, "9000",
                               300, 53, 60, 24, ES_AUTOHSCROLL | ES_NUMBER,
                               IDC_EDIT_PORT, g_font_ui);

        g_btn_connect    = mk_button(hwnd, "Connect",    370, 52, 94, 26, IDC_BTN_CONNECT,    g_font_ui);
        g_btn_disconnect = mk_button(hwnd, "Disconnect", 472, 52, 102, 26, IDC_BTN_DISCONNECT, g_font_ui);

        /* horizontal rule */
        CreateWindowExA(0, "STATIC", "",
                         WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                         18, 88, 576, 2, hwnd, NULL,
                         GetModuleHandleA(NULL), NULL);

        /* server file list */
        mk_label(hwnd, "Files on Server:", 18, 96, 180, 20, g_font_ui);
        g_btn_refresh = mk_button(hwnd, "Refresh List", 200, 94, 108, 24, IDC_BTN_REFRESH, g_font_ui);

        g_list_files = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
                                        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                        18, 122, 576, 168,
                                        hwnd, (HMENU)IDC_LIST_FILES,
                                        GetModuleHandleA(NULL), NULL);
        SendMessageA(g_list_files, WM_SETFONT, (WPARAM)g_font_mono, TRUE);

        /* action buttons */
        g_btn_upload   = mk_button(hwnd, "Upload File",        18, 302, 126, 30, IDC_BTN_UPLOAD,   g_font_ui);
        g_btn_download = mk_button(hwnd, "Download Selected", 154, 302, 160, 30, IDC_BTN_DOWNLOAD, g_font_ui);

        /* progress */
        mk_label(hwnd, "Progress:", 18, 346, 68, 20, g_font_ui);
        g_progress = CreateWindowExA(0, PROGRESS_CLASS, NULL,
                                      WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                      90, 346, 504, 20,
                                      hwnd, (HMENU)IDC_PROGRESS,
                                      GetModuleHandleA(NULL), NULL);
        SendMessage(g_progress, PBM_SETRANGE32, 0, 1000);

        /* log */
        mk_label(hwnd, "Activity Log:", 18, 376, 140, 20, g_font_ui);
        g_log = mk_edit(hwnd, "", 18, 398, 576, 132,
                         WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                         IDC_LOG, g_font_mono);

        /* status bar */
        g_status_bar = CreateWindowExA(0, STATUSCLASSNAMEA, "Not connected.",
                                        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                        0, 0, 0, 0,
                                        hwnd, (HMENU)IDC_STATUS,
                                        GetModuleHandleA(NULL), NULL);

        ui_set_connected(0);
        log_msg("Welcome! Enter server IP and click Connect.");
        break;
    }

    case WM_ERASEBKGND:
    {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_brush_bg);
        return 1;
    }

    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, CLR_BG);
        SetTextColor((HDC)wp, CLR_TEXT);
        return (LRESULT)g_brush_bg;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wp, CLR_PANEL);
        SetTextColor((HDC)wp, CLR_TEXT);
        return (LRESULT)g_brush_panel;

    case WM_SIZE:
        SendMessage(g_status_bar, WM_SIZE, 0, 0);
        break;

    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        switch (id) {
            case IDC_BTN_CONNECT:    do_connect();    break;
            case IDC_BTN_DISCONNECT: do_disconnect(); break;
            case IDC_BTN_REFRESH:    do_refresh();    break;
            case IDC_BTN_UPLOAD:     do_upload();     break;
            case IDC_BTN_DOWNLOAD:   do_download();   break;
        }
        break;
    }

    /* ── messages from worker threads ── */
    case WM_TRANSFER_DONE:
        ui_set_connected(g_sock != INVALID_SOCKET);
        if (g_sock != INVALID_SOCKET) set_status("Ready.");
        break;

    case WM_TRANSFER_PROG:
        SendMessage(g_progress, PBM_SETRANGE32, 0, (LPARAM)lp);
        SendMessage(g_progress, PBM_SETPOS, wp, 0);
        break;

    case WM_LOG_LINE:
    {
        char *m = (char *)lp;
        if (m) { log_msg(m); free(m); }
        break;
    }

    case WM_LIST_READY:
    {
        char *data = (char *)lp;
        SendMessageA(g_list_files, LB_RESETCONTENT, 0, 0);
        if (data && data[0] != '\0') {
            char copy[131072];
            strncpy(copy, data, sizeof(copy) - 1);
            free(data);

            char *tok = strtok(copy, "\n");
            int count = 0;
            while (tok) {
                char fname[MAX_PATH];
                unsigned long long fsz = 0;
                int m = sscanf(tok, "%255s %llu", fname, &fsz);
                if (m >= 1) {
                    char pretty[MAX_PATH + 64];
                    if (m == 2 && fsz >= 1024 * 1024)
                        snprintf(pretty, sizeof(pretty), "%-38s  %.2f MB", fname,
                                 fsz / (1024.0 * 1024.0));
                    else if (m == 2 && fsz >= 1024)
                        snprintf(pretty, sizeof(pretty), "%-38s  %.2f KB", fname,
                                 fsz / 1024.0);
                    else if (m == 2)
                        snprintf(pretty, sizeof(pretty), "%-38s  %llu B", fname, fsz);
                    else
                        snprintf(pretty, sizeof(pretty), "%s", fname);

                    SendMessageA(g_list_files, LB_ADDSTRING, 0, (LPARAM)pretty);
                    count++;
                }
                tok = strtok(NULL, "\n");
            }
            char msg[64];
            snprintf(msg, sizeof(msg), "%d file(s) on server.", count);
            log_msg(msg);
        } else {
            if (data) free(data);
            log_msg("Server has no files yet.");
        }
        break;
    }

    case WM_DESTROY:
        do_disconnect();
        DeleteObject(g_font_ui);
        DeleteObject(g_font_title);
        DeleteObject(g_font_mono);
        DeleteObject(g_brush_bg);
        DeleteObject(g_brush_panel);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    return 0;
}

/* ── WinMain ─────────────────────────────────────────────────────────── */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    (void)hPrev; (void)lpCmd;

    InitCommonControls();
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "FTClient";
    wc.hIcon         = LoadIconA(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassExA(&wc);

    g_hwnd = CreateWindowExA(
        0, "FTClient",
        "TCP File Transfer Client",
        (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX),
        CW_USEDEFAULT, CW_USEDEFAULT,
        618, 592,
        NULL, NULL, hInst, NULL);

    if (!g_hwnd) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "CreateWindow failed with error code: %lu", GetLastError());
        MessageBoxA(NULL, err_msg, "Initialization Error", MB_ICONERROR | MB_OK);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    SetForegroundWindow(g_hwnd);

    MSG message;
    while (GetMessageA(&message, NULL, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return (int)message.wParam;
}
