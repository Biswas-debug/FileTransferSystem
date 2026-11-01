#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <direct.h>

#define PORT 9000
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 8192
#define MAX_LINE 1024
#define CLIENT_DIR "client_files"

int send_all(SOCKET s, const char *data, int len)
{
    int sent = 0;
    while (sent < len)
    {
        int n = send(s, data + sent, len - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

int recv_all(SOCKET s, char *data, int len)
{
    int received = 0;
    while (received < len)
    {
        int n = recv(s, data + received, len - received, 0);
        if (n <= 0) return 0;
        received += n;
    }
    return 1;
}

int recv_line(SOCKET s, char *line, int capacity)
{
    int i = 0;
    char ch;

    while (i < capacity - 1)
    {
        int n = recv(s, &ch, 1, 0);
        if (n <= 0) return 0;
        if (ch == '\n') break;
        if (ch != '\r') line[i++] = ch;
    }

    line[i] = '\0';
    return 1;
}

int valid_filename(const char *name)
{
    if (!name || !*name ||
        strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0)
        return 0;

    if (strchr(name, '/') || strchr(name, '\\') ||
        strchr(name, ':') || strchr(name, '\r') ||
        strchr(name, '\n'))
        return 0;

    return 1;
}

char *selectFile(void)
{
    static char path[MAX_PATH];
    OPENFILENAMEA ofn;

    memset(&ofn, 0, sizeof(ofn));
    path[0] = '\0';

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.lpstrFilter =
        "All Files (*.*)\0*.*\0"
        "Images (*.jpg;*.jpeg;*.png;*.gif;*.bmp)\0"
        "*.jpg;*.jpeg;*.png;*.gif;*.bmp\0"
        "Documents (*.pdf;*.txt;*.docx)\0*.pdf;*.txt;*.docx\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn))
        return path;

    return NULL;
}

int upload_file(SOCKET sock)
{
    char *path = selectFile();

    if (!path)
    {
        printf("File selection cancelled.\n");
        return 1;
    }

    char filename[MAX_PATH];
    const char *base = strrchr(path, '\\');
    base = base ? base + 1 : path;

    if (strlen(base) >= sizeof(filename))
    {
        printf("Filename is too long.\n");
        return 1;
    }

    strcpy(filename, base);

    if (!valid_filename(filename))
    {
        printf("Invalid filename.\n");
        return 1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        printf("Could not open the selected file.\n");
        return 1;
    }

    if (_fseeki64(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        printf("Could not determine file size.\n");
        return 1;
    }

    long long file_size = _ftelli64(fp);
    if (file_size < 0 || _fseeki64(fp, 0, SEEK_SET) != 0)
    {
        fclose(fp);
        printf("Could not read the selected file.\n");
        return 1;
    }

    char size_line[64];
    snprintf(size_line, sizeof(size_line), "%llu\n",
             (unsigned long long)file_size);

    printf("Uploading: %s (%llu bytes)\n",
           filename, (unsigned long long)file_size);

    if (!send_all(sock, "UPLOAD\n", 7) ||
        !send_all(sock, filename, (int)strlen(filename)) ||
        !send_all(sock, "\n", 1) ||
        !send_all(sock, size_line, (int)strlen(size_line)))
    {
        fclose(fp);
        printf("Connection lost.\n");
        return 0;
    }

    char buffer[BUFFER_SIZE];
    unsigned long long remaining = (unsigned long long)file_size;

    while (remaining > 0)
    {
        size_t chunk = remaining > BUFFER_SIZE
                           ? BUFFER_SIZE : (size_t)remaining;
        size_t n = fread(buffer, 1, chunk, fp);

        if (n != chunk || !send_all(sock, buffer, (int)n))
        {
            fclose(fp);
            printf("Upload failed.\n");
            return 0;
        }

        remaining -= n;
    }

    fclose(fp);

    char response[MAX_LINE];
    if (!recv_line(sock, response, sizeof(response)))
    {
        printf("Server connection lost.\n");
        return 0;
    }

    if (strcmp(response, "OK") == 0)
        printf("Upload successful!\n");
    else
        printf("Server response: %s\n", response);

    return 1;
}

int download_file(SOCKET sock)
{
    char filename[MAX_PATH];
    char response[MAX_LINE];

    printf("Enter the exact filename stored on the server: ");

    if (fgets(filename, sizeof(filename), stdin) == NULL)
        return 1;

    filename[strcspn(filename, "\r\n")] = '\0';

    if (!valid_filename(filename))
    {
        printf("Invalid filename.\n");
        return 1;
    }

    if (!send_all(sock, "DOWNLOAD\n", 9) ||
        !send_all(sock, filename, (int)strlen(filename)) ||
        !send_all(sock, "\n", 1))
    {
        printf("Connection lost.\n");
        return 0;
    }

    if (!recv_line(sock, response, sizeof(response)))
    {
        printf("Server connection lost.\n");
        return 0;
    }

    if (strncmp(response, "OK ", 3) != 0)
    {
        printf("Server response: %s\n", response);
        return 1;
    }

    unsigned long long file_size;
    if (sscanf(response + 3, "%llu", &file_size) != 1)
    {
        printf("Invalid server response.\n");
        return 0;
    }

    _mkdir(CLIENT_DIR);

    char output_path[MAX_PATH];
    int written = snprintf(output_path, sizeof(output_path),
                           CLIENT_DIR "\\%s", filename);

    if (written < 0 || written >= (int)sizeof(output_path))
    {
        printf("Output path is too long.\n");
        return 1;
    }

    FILE *fp = fopen(output_path, "wb");
    if (!fp)
    {
        printf("Cannot create the downloaded file.\n");
        return 0;
    }

    char buffer[BUFFER_SIZE];
    unsigned long long remaining = file_size;

    while (remaining > 0)
    {
        int chunk = remaining > BUFFER_SIZE
                        ? BUFFER_SIZE : (int)remaining;

        if (!recv_all(sock, buffer, chunk))
        {
            fclose(fp);
            remove(output_path);
            printf("Download interrupted.\n");
            return 0;
        }

        if (fwrite(buffer, 1, chunk, fp) != (size_t)chunk)
        {
            fclose(fp);
            remove(output_path);
            printf("Could not save the downloaded file.\n");
            return 0;
        }

        remaining -= chunk;
    }

    fclose(fp);
    printf("Download successful!\nSaved to: %s\n", output_path);
    return 1;
}

int list_files(SOCKET sock)
{
    if (!send_all(sock, "LIST\n", 5))
    {
        printf("Connection lost.\n");
        return 0;
    }

    printf("\n--- Files on Server ---\n");
    char line[MAX_LINE];
    int count = 0;
    while (recv_line(sock, line, sizeof(line)))
    {
        if (line[0] == '\0') break;
        char name[MAX_PATH];
        unsigned long long sz = 0;
        if (sscanf(line, "%s %llu", name, &sz) == 2)
        {
            if (sz >= 1024 * 1024)
                printf("  %-30s (%.2f MB)\n", name, sz / (1024.0 * 1024.0));
            else if (sz >= 1024)
                printf("  %-30s (%.2f KB)\n", name, sz / 1024.0);
            else
                printf("  %-30s (%llu Bytes)\n", name, sz);
        }
        else
        {
            printf("  %s\n", line);
        }
        count++;
    }
    if (count == 0)
        printf("  (No files currently on server)\n");
    printf("------------------------\n");
    return 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    WSADATA wsa;
    SOCKET sock;
    struct sockaddr_in server_addr;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("Winsock initialization failed.\n");
        return 1;
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        printf("Socket creation failed.\n");
        WSACleanup();
        return 1;
    }

    char target_ip[128] = "127.0.0.1";
    char ip_input[128];
    printf("Enter Server IP (press Enter for default 127.0.0.1): ");
    if (fgets(ip_input, sizeof(ip_input), stdin))
    {
        ip_input[strcspn(ip_input, "\r\n")] = '\0';
        if (strlen(ip_input) > 0)
        {
            strncpy(target_ip, ip_input, sizeof(target_ip) - 1);
        }
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    unsigned long in_addr = inet_addr(target_ip);
    if (in_addr == INADDR_NONE && strcmp(target_ip, "255.255.255.255") != 0) {
        struct hostent *he = gethostbyname(target_ip);
        if (!he || !he->h_addr_list[0]) {
            printf("Invalid server IP or hostname.\n");
            closesocket(sock);
            WSACleanup();
            return 1;
        }
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
    } else {
        server_addr.sin_addr.s_addr = in_addr;
    }

    printf("Connecting to %s:%d ...\n", target_ip, PORT);
    if (connect(sock, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) == SOCKET_ERROR)
    {
        printf("Could not connect to the server at %s:%d.\n", target_ip, PORT);
        printf("Make sure server.exe is running on that computer and port %d is allowed through firewall.\n", PORT);
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    printf("Connected to TCP File Transfer Server!\n");

    int running = 1;

    while (running)
    {
        char input[32];
        int choice;

        printf("\n===== TCP FILE TRANSFER =====\n");
        printf("1. Upload file (browse computer)\n");
        printf("2. Download file\n");
        printf("3. View files on server\n");
        printf("4. Exit\n");
        printf("Enter choice (1-4): ");

        if (!fgets(input, sizeof(input), stdin))
            break;

        if (sscanf(input, "%d", &choice) != 1)
        {
            printf("Please enter 1, 2, 3, or 4.\n");
            continue;
        }

        switch (choice)
        {
            case 1:
                running = upload_file(sock);
                break;

            case 2:
                running = download_file(sock);
                break;

            case 3:
                running = list_files(sock);
                break;

            case 4:
                send_all(sock, "QUIT\n", 5);
                running = 0;
                printf("Exiting client.\n");
                break;

            default:
                printf("Invalid choice. Try again.\n");
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
