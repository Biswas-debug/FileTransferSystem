# 📁 TCP File Transfer System

A **Multi-Client LAN File Transfer System** built in **pure C language** using **Winsock2 TCP sockets** on Windows.  
Multiple computers on the same Wi-Fi/LAN can upload and download files from a central server — through a **desktop GUI app** or a **web browser**.

---

## 🚀 Features

- ✅ **Multi-client support** — up to 64 clients connected simultaneously using multi-threading (`CreateThread`)
- ✅ **Desktop GUI Client** — Windows native GUI built with Win32 API (pure C, no frameworks)
- ✅ **Console Client** — simple command-line client for upload, download, and list
- ✅ **Web Browser Portal** — anyone on the same Wi-Fi can open `http://<SERVER_IP>:9000` in Chrome/Edge and transfer files
- ✅ **Drag & Drop Upload** — from the browser portal
- ✅ **Live Client Count** — web portal shows how many clients are connected in real time
- ✅ **Security Access Code** — clients must enter the authorization code (`2026`) before accessing, uploading, or downloading files
- ✅ **No internet required** — works entirely on LAN / Wi-Fi

---

## 🛠️ Technology Used

| Component | Technology |
|---|---|
| Language | Pure C (C99) |
| Protocol | TCP/IP (Winsock2) |
| Port | 9000 |
| Compiler | GCC (MinGW64 / MSYS2) |
| GUI | Win32 API (`windows.h`) |
| Web Portal | Built-in HTTP server (served by C server) |
| Multi-threading | `CreateThread` + `CRITICAL_SECTION` |
| OS | Windows 10 / 11 |

---

## 📂 Project Structure

```
FileTransferSystem/
│
├── server.c          # Multi-threaded TCP server + built-in HTTP web server
├── client.c          # Console-based TCP client
├── gui_client.c      # Windows GUI desktop client (Win32 API)
├── web_portal.html   # Browser portal (served automatically by server)
├── compile.bat       # One-click compile script
├── run.bat           # One-click run script (starts server + GUI client)
│
├── server_files/     # Files stored on the server (upload destination)
└── client_files/     # Files downloaded by the client
```

---

## ⚙️ How to Compile

Make sure you have **GCC (MinGW64/MSYS2)** installed.

```bat
compile.bat
```

Or manually:

```bash
# Compile server
gcc -O2 server.c -o server.exe -lws2_32 -static-libgcc

# Compile console client
gcc -O2 client.c -o client.exe -lws2_32 -static-libgcc

# Compile GUI client
gcc -O2 gui_client.c -o gui_client.exe -lws2_32 -mwindows -lcomctl32 -static-libgcc
```

---

## ▶️ How to Run

### Step 1 — Start the Server
Double-click `server.exe` or run:
```bash
server.exe
```
The server will display its **LAN IP address** and the **browser URL**.

### Step 2 — Connect from another computer

**Option A — Web Browser (easiest):**
Open Chrome or Edge on any computer on the same Wi-Fi and go to:
```
http://<SERVER_IP>:9000
```
You can upload and download files directly from the browser — no software needed!

**Option B — GUI Desktop Client:**
Run `gui_client.exe`, enter the server's IP address and port `9000`, click **Connect**.

**Option C — Console Client:**
```bash
client.exe
```
Enter server IP, then choose: `1` Upload, `2` Download, `3` List files.

---

## 🌐 How It Works

```
Client (console/GUI/browser)
        |
        |  TCP Connection (port 9000)
        |
        ▼
   [ SERVER.EXE ]
        |
        |-- Detects HTTP request → serves web_portal.html / JSON / file download
        |
        |-- Detects TCP command → UPLOAD / DOWNLOAD / LIST / QUIT
        |
        |-- Creates a new Thread for each client (multi-threading)
        |
        ▼
   server_files\   (all uploaded files stored here)
```

### Commands (TCP Protocol)
| Command | Description |
|---|---|
| `UPLOAD` | Client sends filename + size + file data to server |
| `DOWNLOAD` | Client requests a file by name; server sends it back |
| `LIST` | Server replies with list of all available files |
| `QUIT` | Client disconnects cleanly |

---

## 📸 Screenshots

> Run the server and open `http://127.0.0.1:9000` in your browser to see the web portal.

---

## 📚 Concepts Demonstrated

- TCP Socket Programming (Winsock2)
- Client-Server Architecture
- Multi-threading with `CreateThread`
- Thread Synchronization with `CRITICAL_SECTION`
- HTTP Protocol implementation from scratch in C
- Win32 GUI Programming
- Binary file transfer over network (chunked sending/receiving)
- LAN networking

---

## 👨‍💻 Author

**Biswas Gupta**  
Computer Networks Lab Project  
Built with ❤️ in pure C
