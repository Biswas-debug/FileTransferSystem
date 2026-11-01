
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <direct.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT        9000
#define BUFFER_SIZE 8192
#define MAX_LINE    1024
#define SERVER_DIR  "server_files"
#define MAX_CLIENTS 64

/* ── small helpers ─────────────────────────────────────────────────────── */

static int send_all(SOCKET s, const char *data, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

static int recv_all(SOCKET s, char *data, int len)
{
    int recv_done = 0;
    while (recv_done < len) {
        int n = recv(s, data + recv_done, len - recv_done, 0);
        if (n <= 0) return 0;
        recv_done += n;
    }
    return 1;
}

static int recv_line(SOCKET s, char *line, int cap)
{
    int i = 0;
    char ch;
    while (i < cap - 1) {
        int n = recv(s, &ch, 1, 0);
        if (n <= 0) return 0;
        if (ch == '\n') break;
        if (ch != '\r') line[i++] = ch;
    }
    line[i] = '\0';
    return 1;
}

static int valid_filename(const char *name)
{
    if (!name || !*name) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    if (strchr(name, '/') || strchr(name, '\\') ||
        strchr(name, ':') || strchr(name, '\r') ||
        strchr(name, '\n'))
        return 0;
    return 1;
}

/* ── keep track of connected clients ───────────────────────────────────── */

static CRITICAL_SECTION g_cs;
static int g_client_count = 0;

static void client_inc(void)
{
    EnterCriticalSection(&g_cs);
    g_client_count++;
    printf("[server] clients connected: %d\n", g_client_count);
    LeaveCriticalSection(&g_cs);
}

static void client_dec(void)
{
    EnterCriticalSection(&g_cs);
    g_client_count--;
    printf("[server] clients connected: %d\n", g_client_count);
    LeaveCriticalSection(&g_cs);
}

/* ── URL Decoder ────────────────────────────────────────────────────────── */
static void url_decode(char *dst, const char *src, int max_len)
{
    int i = 0;
    while (*src && i < max_len - 1)
    {
        if (*src == '%' && src[1] && src[2])
        {
            char hex[3] = { src[1], src[2], '\0' };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        }
        else if (*src == '+')
        {
            dst[i++] = ' ';
            src++;
        }
        else
        {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* ── HTTP Web Portal Handlers ─────────────────────────────────────────── */

static void http_send_html_page(SOCKET client)
{
    FILE *fp = fopen("web_portal.html", "rb");
    if (fp) {
        _fseeki64(fp, 0, SEEK_END);
        long long fsize = _ftelli64(fp);
        _fseeki64(fp, 0, SEEK_SET);

        char header[256];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %lld\r\n"
            "Connection: close\r\n\r\n", fsize);

        if (send_all(client, header, hlen)) {
            char buf[BUFFER_SIZE];
            unsigned long long rem = (unsigned long long)fsize;
            while (rem > 0) {
                size_t chunk = (rem > BUFFER_SIZE) ? BUFFER_SIZE : (size_t)rem;
                size_t n = fread(buf, 1, chunk, fp);
                if (n == 0 || !send_all(client, buf, (int)n)) break;
                rem -= n;
            }
        }
        fclose(fp);
    } else {
        const char *fallback =
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><title>File Server</title></head><body>"
            "<h2>TCP File Server Online</h2><p>web_portal.html not found.</p></body></html>";
        send_all(client, fallback, (int)strlen(fallback));
    }
}

static void http_send_file_list_json(SOCKET client)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), SERVER_DIR "\\*");

    HANDLE hf = FindFirstFileA(pattern, &fd);
    char *json = (char *)malloc(131072);
    if (!json) return;
    strcpy(json, "[");
    int first = 1;

    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            unsigned long long fsz =
                ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;

            char entry[512];
            snprintf(entry, sizeof(entry), "%s{\"name\":\"%s\",\"size\":%llu}",
                     first ? "" : ",", fd.cFileName, fsz);
            first = 0;
            strncat(json, entry, 131071 - strlen(json));
        } while (FindNextFileA(hf, &fd));
        FindClose(hf);
    }
    strncat(json, "]", 131071 - strlen(json));

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", (int)strlen(json));

    send_all(client, header, hlen);
    send_all(client, json, (int)strlen(json));
    free(json);
}

static void http_handle_download(SOCKET client, const char *raw_param)
{
    char filename[MAX_PATH];
    url_decode(filename, raw_param, sizeof(filename));

    if (!valid_filename(filename)) {
        const char *err = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid filename";
        send_all(client, err, (int)strlen(err));
        return;
    }

    char file_path[MAX_PATH];
    snprintf(file_path, sizeof(file_path), SERVER_DIR "\\%s", filename);

    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        const char *err = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nFile not found";
        send_all(client, err, (int)strlen(err));
        return;
    }

    _fseeki64(fp, 0, SEEK_END);
    long long file_size = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Length: %lld\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n", filename, file_size);

    if (send_all(client, header, hlen)) {
        char buffer[BUFFER_SIZE];
        unsigned long long rem = (unsigned long long)file_size;
        while (rem > 0) {
            size_t chunk = (rem > BUFFER_SIZE) ? BUFFER_SIZE : (size_t)rem;
            size_t n = fread(buffer, 1, chunk, fp);
            if (n == 0 || !send_all(client, buffer, (int)n)) break;
            rem -= n;
        }
    }
    fclose(fp);
}

static void http_handle_upload(SOCKET client, const char *raw_name, unsigned long long content_length)
{
    char filename[MAX_PATH];
    url_decode(filename, raw_name, sizeof(filename));

    if (!valid_filename(filename) || content_length == 0) {
        const char *err = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid upload request";
        send_all(client, err, (int)strlen(err));
        return;
    }

    char file_path[MAX_PATH];
    snprintf(file_path, sizeof(file_path), SERVER_DIR "\\%s", filename);

    FILE *fp = fopen(file_path, "wb");
    if (!fp) {
        const char *err = "HTTP/1.1 500 Internal Error\r\nConnection: close\r\n\r\nCannot create file";
        send_all(client, err, (int)strlen(err));
        return;
    }

    char buffer[BUFFER_SIZE];
    unsigned long long rem = content_length;
    int ok = 1;
    while (rem > 0) {
        int chunk = (rem > BUFFER_SIZE) ? BUFFER_SIZE : (int)rem;
        if (!recv_all(client, buffer, chunk) || fwrite(buffer, 1, chunk, fp) != (size_t)chunk) {
            ok = 0; break;
        }
        rem -= chunk;
    }
    fclose(fp);

    if (!ok) {
        remove(file_path);
        const char *err = "HTTP/1.1 500 Internal Error\r\nConnection: close\r\n\r\nUpload failed";
        send_all(client, err, (int)strlen(err));
        return;
    }

    printf("[web] Uploaded via browser: %s (%llu bytes)\n", filename, content_length);

    const char *resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n"
        "{\"status\":\"success\"}";
    send_all(client, resp, (int)strlen(resp));
}

static void handle_http(SOCKET client, const char *peer_ip, const char *request_line)
{
    char method[16] = {0};
    char uri[1024] = {0};
    sscanf(request_line, "%15s %1023s", method, uri);

    unsigned long long content_length = 0;
    char header_line[MAX_LINE];
    while (recv_line(client, header_line, sizeof(header_line))) {
        if (header_line[0] == '\0') break;
        if (_strnicmp(header_line, "Content-Length:", 15) == 0) {
            content_length = _strtoui64(header_line + 15, NULL, 10);
        }
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0) {
            printf("[%s] [web] Browser accessed Web Portal\n", peer_ip);
            http_send_html_page(client);
        } else if (strcmp(uri, "/api/files") == 0) {
            http_send_file_list_json(client);
        } else if (strcmp(uri, "/api/status") == 0) {
            char status_json[256];
            EnterCriticalSection(&g_cs);
            int count = g_client_count;
            LeaveCriticalSection(&g_cs);
            int hlen = snprintf(status_json, sizeof(status_json),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json; charset=utf-8\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n"
                "{\"clients\":%d,\"port\":%d}", count, PORT);
            send_all(client, status_json, hlen);
        } else if (strncmp(uri, "/download?file=", 15) == 0) {
            printf("[%s] [web] Browser downloading: %s\n", peer_ip, uri + 15);
            http_handle_download(client, uri + 15);
        } else {
            const char *nf = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot Found";
            send_all(client, nf, (int)strlen(nf));
        }
    } else if (strcmp(method, "POST") == 0) {
        if (strncmp(uri, "/upload?name=", 13) == 0) {
            http_handle_upload(client, uri + 13, content_length);
        } else {
            const char *nf = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot Found";
            send_all(client, nf, (int)strlen(nf));
        }
    } else if (strcmp(method, "OPTIONS") == 0) {
        const char *cors =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n\r\n";
        send_all(client, cors, (int)strlen(cors));
    }
}

/* ── per-client work ───────────────────────────────────────────────────── */

static void handle_client(SOCKET client, const char *peer_ip)
{
    char command[MAX_LINE];
    char filename[MAX_PATH];
    char buffer[BUFFER_SIZE];

    printf("[%s] session started\n", peer_ip);
    client_inc();

    while (recv_line(client, command, sizeof(command)))
    {
        /* ── Web Browser (HTTP) Support ── */
        if (strncmp(command, "GET ", 4) == 0 ||
            strncmp(command, "POST ", 5) == 0 ||
            strncmp(command, "OPTIONS ", 8) == 0)
        {
            handle_http(client, peer_ip, command);
            break;
        }

        /* ── UPLOAD ── */
        if (strcmp(command, "UPLOAD") == 0)
        {
            char size_line[64];

            if (!recv_line(client, filename, sizeof(filename)) ||
                !recv_line(client, size_line,  sizeof(size_line)))
                break;

            if (!valid_filename(filename)) {
                send_all(client, "ERR Invalid filename\n", 21);
                break;
            }

            char *end;
            unsigned long long size = _strtoui64(size_line, &end, 10);
            if (*size_line == '\0' || *end != '\0') {
                send_all(client, "ERR Invalid size\n", 17);
                break;
            }

            char path[MAX_PATH];
            snprintf(path, sizeof(path), SERVER_DIR "\\%s", filename);

            FILE *fp = fopen(path, "wb");
            if (!fp) {
                send_all(client, "ERR Cannot create file\n", 23);
                break;
            }

            unsigned long long remaining = size;
            int ok = 1;

            while (remaining > 0) {
                int chunk = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : (int)remaining;
                if (!recv_all(client, buffer, chunk) ||
                    fwrite(buffer, 1, chunk, fp) != (size_t)chunk)
                {
                    ok = 0; break;
                }
                remaining -= chunk;
            }

            fclose(fp);

            if (!ok) {
                remove(path);
                break;
            }

            printf("[%s] upload OK  : %s (%llu bytes)\n", peer_ip, filename, size);
            if (!send_all(client, "OK\n", 3)) break;
        }

        /* ── DOWNLOAD ── */
        else if (strcmp(command, "DOWNLOAD") == 0)
        {
            if (!recv_line(client, filename, sizeof(filename))) break;

            if (!valid_filename(filename)) {
                if (!send_all(client, "ERR Invalid filename\n", 21)) break;
                continue;
            }

            char path[MAX_PATH];
            snprintf(path, sizeof(path), SERVER_DIR "\\%s", filename);

            FILE *fp = fopen(path, "rb");
            if (!fp) {
                if (!send_all(client, "ERR File not found\n", 19)) break;
                continue;
            }

            if (_fseeki64(fp, 0, SEEK_END) != 0) {
                fclose(fp);
                send_all(client, "ERR Cannot read file\n", 21);
                continue;
            }

            long long file_size = _ftelli64(fp);
            if (file_size < 0 || _fseeki64(fp, 0, SEEK_SET) != 0) {
                fclose(fp);
                send_all(client, "ERR Cannot read file\n", 21);
                continue;
            }

            char response[64];
            snprintf(response, sizeof(response), "OK %llu\n",
                     (unsigned long long)file_size);

            if (!send_all(client, response, (int)strlen(response))) {
                fclose(fp); break;
            }

            unsigned long long remaining = (unsigned long long)file_size;
            int ok = 1;

            while (remaining > 0) {
                size_t chunk = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : (size_t)remaining;
                size_t n = fread(buffer, 1, chunk, fp);
                if (n != chunk || !send_all(client, buffer, (int)n)) {
                    ok = 0; break;
                }
                remaining -= n;
            }

            fclose(fp);
            if (!ok) break;

            printf("[%s] download OK: %s (%llu bytes)\n",
                   peer_ip, filename, (unsigned long long)file_size);
        }

        /* ── LIST ── */
        else if (strcmp(command, "LIST") == 0)
        {
            WIN32_FIND_DATAA fd;
            char pattern[MAX_PATH];
            snprintf(pattern, sizeof(pattern), SERVER_DIR "\\*");

            HANDLE hf = FindFirstFileA(pattern, &fd);
            char list_buf[65536];
            int  list_len = 0;

            if (hf != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    unsigned long long fsz =
                        ((unsigned long long)fd.nFileSizeHigh << 32) |
                         fd.nFileSizeLow;
                    int written = snprintf(list_buf + list_len,
                                          sizeof(list_buf) - list_len,
                                          "%s %llu\n", fd.cFileName, fsz);
                    if (written > 0) list_len += written;
                } while (FindNextFileA(hf, &fd));
                FindClose(hf);
            }

            /* terminate with a blank line so client knows it's done */
            if (list_len + 2 < (int)sizeof(list_buf)) {
                list_buf[list_len++] = '\n';
                list_buf[list_len]   = '\0';
            }

            if (list_len == 0) {
                send_all(client, "\n", 1);
            } else {
                send_all(client, list_buf, list_len);
            }
        }

        /* ── QUIT ── */
        else if (strcmp(command, "QUIT") == 0)
        {
            break;
        }

        /* ── unknown ── */
        else
        {
            if (!send_all(client, "ERR Unknown command\n", 20)) break;
        }
    }

    printf("[%s] session ended\n", peer_ip);
    client_dec();
}

/* ── thread entry ─────────────────────────────────────────────────────── */

typedef struct {
    SOCKET      sock;
    char        ip[64];
} ClientArg;

static DWORD WINAPI client_thread(LPVOID arg)
{
    ClientArg *ca = (ClientArg *)arg;
    handle_client(ca->sock, ca->ip);
    closesocket(ca->sock);
    free(ca);
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────────── */

static void print_server_ips(void)
{
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0)
    {
        printf("  Server Hostname: %s\n", hostname);
        struct hostent *he = gethostbyname(hostname);
        if (he != NULL)
        {
            printf("  IP to enter in GUI Client on other PCs:\n");
            for (int i = 0; he->h_addr_list[i] != NULL; i++)
            {
                struct in_addr addr;
                memcpy(&addr, he->h_addr_list[i], sizeof(struct in_addr));
                printf("    -> %s (Port %d)\n", inet_ntoa(addr), PORT);
            }
            printf("\n  Web Browser Portal URL (open in Chrome / Edge / Phone):\n");
            for (int i = 0; he->h_addr_list[i] != NULL; i++)
            {
                struct in_addr addr;
                memcpy(&addr, he->h_addr_list[i], sizeof(struct in_addr));
                printf("    -> http://%s:%d\n", inet_ntoa(addr), PORT);
            }
        }
    }
    printf("\n  On THIS LAPTOP, open in browser:\n");
    printf("    -> http://127.0.0.1:%d\n", PORT);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ensure working directory is always the folder where server.exe is located */
    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)) > 0) {
        char *last_slash = strrchr(exe_path, '\\');
        if (last_slash) {
            *last_slash = '\0';
            _chdir(exe_path);
        }
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Winsock initialization failed.\n");
        return 1;
    }

    InitializeCriticalSection(&g_cs);
    _mkdir(SERVER_DIR);

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        printf("Socket creation failed.\n");
        WSACleanup();
        return 1;
    }

    /* allow quick restart without waiting for TIME_WAIT */
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(PORT);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("Bind failed. Port %d already in use?\n", PORT);
        closesocket(server);
        WSACleanup();
        return 1;
    }

    if (listen(server, MAX_CLIENTS) == SOCKET_ERROR) {
        printf("Listen failed.\n");
        closesocket(server);
        WSACleanup();
        return 1;
    }

    printf("==============================================================\n");
    printf("        TCP FILE TRANSFER SERVER (Multi-Computer Support)     \n");
    printf("==============================================================\n");
    printf("  Port: %d | Max Concurrent Clients: %d\n", PORT, MAX_CLIENTS);
    printf("  Storage Folder: .\\%s\\\n\n", SERVER_DIR);
    print_server_ips();
    printf("==============================================================\n");
    printf("  Waiting for client connections...\n\n");

    for (;;)
    {
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);
        SOCKET client = accept(server, (struct sockaddr *)&client_addr, &addr_len);
        if (client == INVALID_SOCKET) {
            printf("Accept failed (error %d).\n", WSAGetLastError());
            continue;
        }

        ClientArg *ca = (ClientArg *)malloc(sizeof(ClientArg));
        if (!ca) {
            closesocket(client);
            continue;
        }

        ca->sock = client;
        snprintf(ca->ip, sizeof(ca->ip), "%s:%d",
                 inet_ntoa(client_addr.sin_addr),
                 ntohs(client_addr.sin_port));

        printf("[server] new connection from %s\n", ca->ip);

        HANDLE t = CreateThread(NULL, 0, client_thread, ca, 0, NULL);
        if (t) CloseHandle(t);
        else {
            printf("[server] CreateThread failed\n");
            closesocket(client);
            free(ca);
        }
    }

    DeleteCriticalSection(&g_cs);
    closesocket(server);
    WSACleanup();
    return 0;
}
