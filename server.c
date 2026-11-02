
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <windows.h>
#include <urlmon.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <direct.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "urlmon.lib")

#define PORT        9000
#define BUFFER_SIZE 8192
#define MAX_LINE    1024
#define SERVER_DIR  "server_files"
#define MAX_CLIENTS 64
#define ACCESS_CODE "2026"    /* access code required to use the server */

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

/* ── Internet / Web Downloader ─────────────────────────────────────────── */
static int download_from_internet(const char *raw_url, const char *suggested_name, char *saved_filename, int max_name_len)
{
    char url[2048];
    url_decode(url, raw_url, sizeof(url));

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        printf("[server] Invalid URL scheme (must be http:// or https://): %s\n", url);
        return 0;
    }

    char fname[MAX_PATH] = {0};
    if (suggested_name && suggested_name[0]) {
        url_decode(fname, suggested_name, sizeof(fname));
    }

    /* if no suggested name, deduce from URL */
    if (!valid_filename(fname)) {
        char clean_url[2048];
        strncpy(clean_url, url, sizeof(clean_url) - 1);
        clean_url[sizeof(clean_url) - 1] = '\0';

        char *query_part = strchr(clean_url, '?');
        if (query_part) *query_part = '\0';

        char *last_slash = strrchr(clean_url, '/');
        if (last_slash && strlen(last_slash + 1) > 0) {
            strncpy(fname, last_slash + 1, sizeof(fname) - 1);
        } else {
            snprintf(fname, sizeof(fname), "download_%u.bin", (unsigned int)GetTickCount());
        }
    }

    if (!valid_filename(fname)) {
        snprintf(fname, sizeof(fname), "download_%u.bin", (unsigned int)GetTickCount());
    }

    char file_path[MAX_PATH];
    snprintf(file_path, sizeof(file_path), SERVER_DIR "\\%s", fname);

    printf("[server] Downloading from Web URL: %s -> %s\n", url, file_path);

    HRESULT hr = URLDownloadToFileA(NULL, url, file_path, 0, NULL);
    if (SUCCEEDED(hr)) {
        printf("[server] Successfully downloaded from Web: %s\n", fname);
        if (saved_filename && max_name_len > 0) {
            strncpy(saved_filename, fname, max_name_len - 1);
            saved_filename[max_name_len - 1] = '\0';
        }
        return 1;
    } else {
        printf("[server] Web download failed for %s (error 0x%08lX)\n", url, (unsigned long)hr);
        return 0;
    }
}

/* ── Web / Cloud Smart Search Fallback ─────────────────────────────────── */
static int search_web_and_download(const char *raw_query, char *saved_filename, int max_name_len)
{
    char query[512];
    url_decode(query, raw_query, sizeof(query));

    /* 1. If user typed or pasted an HTTP/HTTPS URL, download directly! */
    if (strncmp(query, "http://", 7) == 0 || strncmp(query, "https://", 8) == 0) {
        printf("[server] Query is a direct web URL, delegating to downloader: %s\n", query);
        return download_from_internet(query, NULL, saved_filename, max_name_len);
    }

    /* Convert to lowercase for keyword matching */
    char lower_q[512];
    int qlen = (int)strlen(query);
    for (int i = 0; i < qlen && i < 511; i++) {
        lower_q[i] = (char)tolower((unsigned char)query[i]);
    }
    lower_q[qlen] = '\0';

    /* 2. Direct instant datasets & common file formats */
    if (strstr(lower_q, "iris") != NULL) {
        printf("[server] Direct dataset match: iris.csv\n");
        return download_from_internet("https://raw.githubusercontent.com/mwaskom/seaborn-data/master/iris.csv", "iris.csv", saved_filename, max_name_len);
    }
    if (strstr(lower_q, "titanic") != NULL) {
        printf("[server] Direct dataset match: titanic.csv\n");
        return download_from_internet("https://raw.githubusercontent.com/datasciencedojo/datasets/master/titanic.csv", "titanic.csv", saved_filename, max_name_len);
    }
    if (strstr(lower_q, "diabetes") != NULL) {
        printf("[server] Direct dataset match: diabetes.csv\n");
        return download_from_internet("https://raw.githubusercontent.com/plotly/datasets/master/diabetes.csv", "diabetes.csv", saved_filename, max_name_len);
    }
    if (strstr(lower_q, "sample.pdf") != NULL || strstr(lower_q, "sample pdf") != NULL || strstr(lower_q, "dummy pdf") != NULL || strcmp(lower_q, "pdf") == 0) {
        printf("[server] Direct PDF match: sample_document.pdf\n");
        return download_from_internet("https://www.w3.org/WAI/ER/tests/xhtml/testfiles/resources/pdf/dummy.pdf", "sample_document.pdf", saved_filename, max_name_len);
    }
    if (strstr(lower_q, "sample.csv") != NULL || strstr(lower_q, "sample dataset") != NULL || strstr(lower_q, "dataset.csv") != NULL || strcmp(lower_q, "dataset") == 0) {
        printf("[server] Direct dataset match: sample_dataset.csv\n");
        return download_from_internet("https://raw.githubusercontent.com/mwaskom/seaborn-data/master/iris.csv", "sample_dataset.csv", saved_filename, max_name_len);
    }
    if (strstr(lower_q, "cheat sheet") != NULL || strstr(lower_q, "c programming") != NULL) {
        printf("[server] Direct programming reference match\n");
        return download_from_internet("https://raw.githubusercontent.com/ashishb/c-cheat-sheet/master/c-cheat-sheet.txt", "c_programming_notes.txt", saved_filename, max_name_len);
    }

    /* 3. General topic / internet search fallback */
    char clean[256];
    int j = 0;
    for (int i = 0; query[i] && j < 250; i++) {
        char c = query[i];
        if (c == ' ' || c == '+') clean[j++] = '_';
        else if (c != '.' && c != '/' && c != '\\') clean[j++] = c;
    }
    clean[j] = '\0';

    char *sub = strstr(clean, "_photo");
    if (sub) *sub = '\0';
    sub = strstr(clean, "_image");
    if (sub) *sub = '\0';
    sub = strstr(clean, "_pic");
    if (sub) *sub = '\0';

    if (strlen(clean) == 0) return 0;

    printf("[server] Auto-searching Web for: %s (clean: %s)\n", query, clean);

    /* Step 1: OpenSearch resolution to get exact Wikipedia/Wikimedia title */
    char open_url[512];
    snprintf(open_url, sizeof(open_url), "https://en.wikipedia.org/w/api.php?action=opensearch&search=%s&limit=1&format=json", clean);

    char temp1[MAX_PATH];
    snprintf(temp1, sizeof(temp1), "temp_open_%u.json", (unsigned int)GetTickCount());

    char resolved_title[256];
    strncpy(resolved_title, clean, sizeof(resolved_title) - 1);
    resolved_title[sizeof(resolved_title) - 1] = '\0';

    HRESULT hr1 = URLDownloadToFileA(NULL, open_url, temp1, 0, NULL);
    if (SUCCEEDED(hr1)) {
        FILE *fpop = fopen(temp1, "rb");
        if (fpop) {
            char buf_op[2048] = {0};
            fread(buf_op, 1, sizeof(buf_op) - 1, fpop);
            fclose(fpop);
            remove(temp1);

            char *p = strstr(buf_op, ",[\"");
            if (p) {
                p += 3;
                int i = 0;
                while (*p && *p != '\"' && i < 250) {
                    if (*p == ' ') resolved_title[i++] = '_';
                    else resolved_title[i++] = *p;
                    p++;
                }
                resolved_title[i] = '\0';
            }
        }
    } else {
        remove(temp1);
    }

    printf("[server] Resolved article title: %s\n", resolved_title);

    /* Step 2: Download summary JSON */
    char summary_url[512];
    snprintf(summary_url, sizeof(summary_url), "https://en.wikipedia.org/api/rest_v1/page/summary/%s", resolved_title);

    char temp_json[MAX_PATH];
    snprintf(temp_json, sizeof(temp_json), "temp_search_%u.json", (unsigned int)GetTickCount());

    HRESULT hr = URLDownloadToFileA(NULL, summary_url, temp_json, 0, NULL);
    if (!SUCCEEDED(hr)) {
        printf("[server] Web search query failed for %s\n", summary_url);
        return 0;
    }

    FILE *fp = fopen(temp_json, "rb");
    if (!fp) return 0;

    char *buf = (char *)malloc(65536);
    if (!buf) { fclose(fp); remove(temp_json); return 0; }
    size_t nread = fread(buf, 1, 65535, fp);
    buf[nread] = '\0';
    fclose(fp);
    remove(temp_json);

    int success = 0;
    char downloaded_name[MAX_PATH] = {0};

    /* Check if there is an image thumbnail */
    char *src_pos = strstr(buf, "\"source\":\"");
    if (src_pos) {
        src_pos += 10;
        char img_url[1024] = {0};
        int k = 0;
        while (*src_pos && *src_pos != '\"' && k < (int)sizeof(img_url) - 1) {
            img_url[k++] = *src_pos++;
        }
        img_url[k] = '\0';

        char out_name[MAX_PATH];
        snprintf(out_name, sizeof(out_name), "%s.png", resolved_title);

        char out_path[MAX_PATH];
        snprintf(out_path, sizeof(out_path), SERVER_DIR "\\%s", out_name);

        printf("[server] Downloading found image: %s -> %s\n", img_url, out_path);
        HRESULT ihr = URLDownloadToFileA(NULL, img_url, out_path, 0, NULL);
        if (SUCCEEDED(ihr)) {
            printf("[server] Auto-downloaded web file OK: %s\n", out_name);
            strncpy(downloaded_name, out_name, sizeof(downloaded_name) - 1);
            success = 1;
        }
    }

    /* Also ALWAYS generate rich notes document if extract is present */
    char *ext_pos = strstr(buf, "\"extract\":\"");
    if (ext_pos) {
        ext_pos += 11;
        char notes_name[MAX_PATH];
        snprintf(notes_name, sizeof(notes_name), "%s_notes.txt", resolved_title);

        char notes_path[MAX_PATH];
        snprintf(notes_path, sizeof(notes_path), SERVER_DIR "\\%s", notes_name);

        FILE *out_fp = fopen(notes_path, "wb");
        if (out_fp) {
            fprintf(out_fp, "==============================================================\r\n");
            fprintf(out_fp, "  TOPIC: %s\r\n", query);
            fprintf(out_fp, "  SOURCE: Online Encyclopedia / Internet Search Fallback\r\n");
            fprintf(out_fp, "==============================================================\r\n\r\n");

            while (*ext_pos) {
                if (*ext_pos == '\"' && *(ext_pos - 1) != '\\') {
                    break;
                }
                if (*ext_pos == '\\') {
                    if (*(ext_pos + 1) == 'n') {
                        fputc('\r', out_fp);
                        fputc('\n', out_fp);
                        ext_pos += 2;
                        continue;
                    } else if (*(ext_pos + 1) == '\"') {
                        fputc('\"', out_fp);
                        ext_pos += 2;
                        continue;
                    } else if (*(ext_pos + 1) == 'u' && strncmp(ext_pos, "\\u0027", 6) == 0) {
                        fputc('\'', out_fp);
                        ext_pos += 6;
                        continue;
                    } else if (*(ext_pos + 1) == '\\') {
                        fputc('\\', out_fp);
                        ext_pos += 2;
                        continue;
                    }
                }
                fputc(*ext_pos, out_fp);
                ext_pos++;
            }
            fprintf(out_fp, "\r\n\r\n--------------------------------------------------------------\r\n");
            fprintf(out_fp, "Reference Link: https://en.wikipedia.org/wiki/%s\r\n", resolved_title);
            fprintf(out_fp, "Downloaded to Server LAN Repository.\r\n");
            fclose(out_fp);

            printf("[server] Auto-generated web notes document OK: %s\n", notes_name);
            if (!success) {
                strncpy(downloaded_name, notes_name, sizeof(downloaded_name) - 1);
                success = 1;
            }
        }
    }

    if (success && saved_filename && max_name_len > 0) {
        strncpy(saved_filename, downloaded_name, max_name_len - 1);
        saved_filename[max_name_len - 1] = '\0';
    }

    free(buf);
    return success;
}

/* ── HTTP Web Portal Handlers ─────────────────────────────────────────── */

/* extract ?auth=XXXX from a URI query string */
static int http_check_auth(const char *uri)
{
    const char *p = strstr(uri, "auth=");
    if (!p) return 0;
    p += 5;  /* skip "auth=" */
    /* extract value until '&' or end */
    char code[64];
    int i = 0;
    while (p[i] && p[i] != '&' && i < 63) {
        code[i] = p[i];
        i++;
    }
    code[i] = '\0';
    return (strcmp(code, ACCESS_CODE) == 0);
}

static void http_send_unauthorized(SOCKET client)
{
    const char *resp =
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n"
        "{\"error\":\"Invalid access code\"}";
    send_all(client, resp, (int)strlen(resp));
}

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
        /* ── Auto-Search Fallback: If not found on local server, search Google/Web! ── */
        char found_name[MAX_PATH] = {0};
        if (search_web_and_download(filename, found_name, sizeof(found_name))) {
            snprintf(file_path, sizeof(file_path), SERVER_DIR "\\%s", found_name);
            fp = fopen(file_path, "rb");
        }
    }

    if (!fp) {
        const char *err = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nFile not found on server or web search";
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
        } else if (strncmp(uri, "/api/auth?code=", 15) == 0) {
            /* verify access code from browser login */
            char submitted[64];
            url_decode(submitted, uri + 15, sizeof(submitted));
            /* trim at '&' if present */
            char *amp = strchr(submitted, '&');
            if (amp) *amp = '\0';
            if (strcmp(submitted, ACCESS_CODE) == 0) {
                printf("[%s] [web] Access code verified OK\n", peer_ip);
                const char *ok_resp =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n"
                    "{\"status\":\"ok\"}";
                send_all(client, ok_resp, (int)strlen(ok_resp));
            } else {
                printf("[%s] [web] Access code REJECTED\n", peer_ip);
                http_send_unauthorized(client);
            }
        } else if (strncmp(uri, "/api/files", 10) == 0) {
            if (!http_check_auth(uri)) { http_send_unauthorized(client); }
            else http_send_file_list_json(client);
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
        } else if (strncmp(uri, "/download?", 10) == 0) {
            if (!http_check_auth(uri)) { http_send_unauthorized(client); }
            else {
                const char *fp = strstr(uri, "file=");
                if (fp) {
                    fp += 5;
                    /* extract filename until '&' */
                    char fname_buf[MAX_PATH];
                    int k = 0;
                    while (fp[k] && fp[k] != '&' && k < MAX_PATH - 1) { fname_buf[k] = fp[k]; k++; }
                    fname_buf[k] = '\0';
                    printf("[%s] [web] Browser downloading: %s\n", peer_ip, fname_buf);
                    http_handle_download(client, fname_buf);
                }
            }
        } else if (strncmp(uri, "/api/fetch?", 11) == 0) {
            if (!http_check_auth(uri)) { http_send_unauthorized(client); }
            else {
                char raw_url[2048] = {0};
                char raw_name[MAX_PATH] = {0};
                const char *up = strstr(uri, "url=");
                if (up) {
                    up += 4;
                    int k = 0;
                    while (up[k] && up[k] != '&' && k < (int)sizeof(raw_url) - 1) { raw_url[k] = up[k]; k++; }
                    raw_url[k] = '\0';
                }
                const char *np = strstr(uri, "name=");
                if (np) {
                    np += 5;
                    int k = 0;
                    while (np[k] && np[k] != '&' && k < (int)sizeof(raw_name) - 1) { raw_name[k] = np[k]; k++; }
                    raw_name[k] = '\0';
                }

                char saved_name[MAX_PATH] = {0};
                if (download_from_internet(raw_url, raw_name, saved_name, sizeof(saved_name))) {
                    char resp[512];
                    int hlen = snprintf(resp, sizeof(resp),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Connection: close\r\n\r\n"
                        "{\"status\":\"success\",\"filename\":\"%s\"}", saved_name);
                    send_all(client, resp, hlen);
                } else {
                    const char *resp =
                        "HTTP/1.1 500 Internal Error\r\n"
                        "Content-Type: application/json\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Connection: close\r\n\r\n"
                        "{\"error\":\"Failed to download file from the provided web URL\"}";
                    send_all(client, resp, (int)strlen(resp));
                }
            }
        } else if (strncmp(uri, "/api/search?", 12) == 0) {
            if (!http_check_auth(uri)) { http_send_unauthorized(client); }
            else {
                char raw_q[256] = {0};
                const char *qp = strstr(uri, "q=");
                if (qp) {
                    qp += 2;
                    int k = 0;
                    while (qp[k] && qp[k] != '&' && k < (int)sizeof(raw_q) - 1) { raw_q[k] = qp[k]; k++; }
                    raw_q[k] = '\0';
                }

                char saved_name[MAX_PATH] = {0};
                if (search_web_and_download(raw_q, saved_name, sizeof(saved_name))) {
                    char resp[512];
                    int hlen = snprintf(resp, sizeof(resp),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Connection: close\r\n\r\n"
                        "{\"status\":\"success\",\"filename\":\"%s\"}", saved_name);
                    send_all(client, resp, hlen);
                } else {
                    const char *resp =
                        "HTTP/1.1 404 Not Found\r\n"
                        "Content-Type: application/json\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Connection: close\r\n\r\n"
                        "{\"error\":\"Could not find files on web for this search\"}";
                    send_all(client, resp, (int)strlen(resp));
                }
            }
        } else {
            const char *nf = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot Found";
            send_all(client, nf, (int)strlen(nf));
        }
    } else if (strcmp(method, "POST") == 0) {
        if (strncmp(uri, "/upload?", 8) == 0) {
            if (!http_check_auth(uri)) { http_send_unauthorized(client); }
            else {
                const char *np = strstr(uri, "name=");
                if (np) {
                    np += 5;
                    char name_buf[MAX_PATH];
                    int k = 0;
                    while (np[k] && np[k] != '&' && k < MAX_PATH - 1) { name_buf[k] = np[k]; k++; }
                    name_buf[k] = '\0';
                    http_handle_upload(client, name_buf, content_length);
                }
            }
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
            goto session_end;
        }

        /* ── AUTH (must be the first TCP command) ── */
        if (strncmp(command, "AUTH ", 5) == 0)
        {
            const char *code = command + 5;
            if (strcmp(code, ACCESS_CODE) == 0) {
                printf("[%s] access code verified OK\n", peer_ip);
                send_all(client, "AUTH_OK\n", 8);
                break;  /* break out, enter authenticated command loop below */
            } else {
                printf("[%s] access code REJECTED\n", peer_ip);
                send_all(client, "AUTH_FAIL\n", 10);
                goto session_end;
            }
        }

        /* any other command without AUTH first = reject */
        send_all(client, "ERR Auth required\n", 18);
        goto session_end;
    }

    /* ── authenticated command loop (TCP clients only) ── */
    while (recv_line(client, command, sizeof(command)))
    {

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
                /* ── Auto-Search Fallback: If not found on local server, search Google/Web! ── */
                char found_name[MAX_PATH] = {0};
                if (search_web_and_download(filename, found_name, sizeof(found_name))) {
                    snprintf(path, sizeof(path), SERVER_DIR "\\%s", found_name);
                    fp = fopen(path, "rb");
                }
            }

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

        /* ── FETCH (Download from Web / Cloud URL into server_files) ── */
        else if (strcmp(command, "FETCH") == 0)
        {
            char target_url[2048] = {0};
            if (!recv_line(client, target_url, sizeof(target_url))) break;

            char saved_name[MAX_PATH] = {0};
            if (download_from_internet(target_url, NULL, saved_name, sizeof(saved_name))) {
                char ok_msg[MAX_PATH + 32];
                snprintf(ok_msg, sizeof(ok_msg), "OK %s\n", saved_name);
                send_all(client, ok_msg, (int)strlen(ok_msg));
            } else {
                send_all(client, "ERR Web download failed\n", 24);
            }
        }

        /* ── SEARCH (Smart Google/Web search by topic/name without typing URL) ── */
        else if (strcmp(command, "SEARCH") == 0)
        {
            char search_query[256] = {0};
            if (!recv_line(client, search_query, sizeof(search_query))) break;

            char saved_name[MAX_PATH] = {0};
            if (search_web_and_download(search_query, saved_name, sizeof(saved_name))) {
                char ok_msg[MAX_PATH + 32];
                snprintf(ok_msg, sizeof(ok_msg), "OK %s\n", saved_name);
                send_all(client, ok_msg, (int)strlen(ok_msg));
            } else {
                send_all(client, "ERR Web search failed to find file\n", 35);
            }
        }

        /* ── unknown ── */
        else
        {
            if (!send_all(client, "ERR Unknown command\n", 20)) break;
        }
    }

    printf("[%s] session ended\n", peer_ip);
session_end:
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

    /* set browser user-agent so web downloads are fast and never throttled */
    const char *custom_ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) FileTransferSystem/1.0";
    UrlMkSetSessionOption(URLMON_OPTION_USERAGENT, (LPVOID)custom_ua, (DWORD)strlen(custom_ua), 0);

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
    printf("  Access Code: %s\n", ACCESS_CODE);
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
