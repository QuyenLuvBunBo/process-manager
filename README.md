# Linux Process Manager

A fullstack Linux process management system built for an OS course assignment. Demonstrates real Linux kernel APIs (`fork`, `exec`, `wait`, `kill`, `nice`, `/proc`) through three layers: a C core program, a Python REST API, and a web dashboard.

---

## Project Structure

```
process-manager/
├── backend/
│   ├── process_manager.c   # Core C program — Linux API demonstrations
│   ├── process_manager     # Compiled binary (after build)
│   ├── server.py           # Flask REST API server
│   └── requirements.txt    # Python dependencies
└── frontend/
    └── index.html          # Web dashboard (single-file, no build step)
```

---

## Quick Start

### 1. Compile the C program

```bash
cd backend
gcc -o process_manager process_manager.c
```

### 2. Install Python dependencies

```bash
pip install -r requirements.txt 
```

### 3. Start the API server

```bash
python3 server.py
# Server runs on http://localhost:5000
```

### 4. Open the dashboard

Open `frontend/index.html` in any browser. No build step required.

---

## Components

### `process_manager.c` — Core Linux API Demo

A standalone C program that directly calls Linux system APIs to demonstrate process management concepts.

| Command | Description |
|---|---|
| `./process_manager demo` | Full lifecycle: fork → exec → pause → resume → kill → wait |
| `./process_manager list` | List all processes from `/proc` filesystem |
| `./process_manager info` | System info (PID, load avg, memory) |
| `./process_manager tree <pid>` | Process tree from a given root PID |
| `./process_manager create <program>` | Fork + exec a program, then wait |
| `./process_manager kill <pid>` | Send SIGTERM to a process |
| `./process_manager priority <pid> <n>` | Change nice value (-20 to +19) |

**Linux APIs used:**

| API | Purpose |
|---|---|
| `fork()` | Duplicate the current process |
| `execvp()` | Replace process image with a new program |
| `wait()` / `waitpid()` | Block until child exits; reap zombies |
| `kill(pid, sig)` | Send a signal to a process |
| `setpriority()` / `getpriority()` | Read and set scheduling nice value |
| `getpid()` / `getppid()` | Query current and parent PID |
| `sigaction()` | Register custom signal handlers |
| `/proc/<pid>/stat` | Read process state directly from the kernel |
| `/proc/<pid>/status` | Read memory usage (VmRSS) |
| `/proc/loadavg` | System load averages |
| `/proc/meminfo` | System memory information |

---

### `server.py` — Flask REST API

Wraps Linux process management into a JSON API so the web dashboard can interact with the OS.

#### Endpoints

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/api/processes` | List all processes. Supports `?sort=` and `?filter=` |
| `GET` | `/api/processes/<pid>` | Single process details + raw `/proc` stat |
| `GET` | `/api/processes/<pid>/children` | Immediate child processes |
| `POST` | `/api/processes/create` | Fork + exec a new process |
| `POST` | `/api/processes/<pid>/kill` | Send a signal (`SIGTERM`, `SIGKILL`, etc.) |
| `POST` | `/api/processes/<pid>/priority` | Change nice value |
| `GET` | `/api/system` | CPU, memory, swap, load averages |
| `GET` | `/api/tree` | Hierarchical process tree as JSON |
| `POST` | `/api/demo/lifecycle` | Run the compiled C demo and return its output |
| `GET` | `/api/health` | Health check |

#### Example: Create a process

```bash
curl -X POST http://localhost:5000/api/processes/create \
  -H "Content-Type: application/json" \
  -d '{"command": "sleep 30"}'
```

```json
{ "success": true, "pid": 1234, "command": "sleep 30" }
```

#### Example: Send a signal

```bash
curl -X POST http://localhost:5000/api/processes/1234/kill \
  -H "Content-Type: application/json" \
  -d '{"signal": "SIGTERM"}'
```

#### Example: Change priority

```bash
curl -X POST http://localhost:5000/api/processes/1234/priority \
  -H "Content-Type: application/json" \
  -d '{"nice": 10}'
```

---

### `frontend/index.html` — Web Dashboard

A single-file web UI with no build dependencies.

**Pages:**
- **Dashboard** — Live CPU, memory, load average, and top processes by memory
- **Processes** — Searchable, sortable full process table with per-process actions
- **Process Tree** — Hierarchical tree view rooted at any PID
- **Lifecycle Demo** — Runs the compiled C binary and displays annotated output
- **About / API** — Architecture overview and endpoint reference

**Features:**
- Click any process row to open a detail panel
- Send signals (SIGTERM, SIGKILL, SIGSTOP, SIGCONT, SIGHUP, SIGINT) from the UI
- Change nice value from the detail panel
- Fork + exec new processes from the modal launcher
- Auto-refreshes system stats every 3 seconds
- Shows raw `/proc/<pid>/stat` content in detail panel

---

## Process Lifecycle

```
fork()
  │
  ▼
READY ──► RUNNING ──► WAITING (I/O)
              │             │
              │◄────────────┘
              │
           SIGSTOP
              │
              ▼
           STOPPED ──► SIGCONT ──► RUNNING
              │
           SIGTERM / SIGKILL
              │
              ▼
           ZOMBIE ──► waitpid() ──► DEAD
```

---

## Signals Reference

| Signal | Number | Description | Catchable |
|---|---|---|---|
| `SIGHUP` | 1 | Hangup / reload config | Yes |
| `SIGINT` | 2 | Ctrl+C interrupt | Yes |
| `SIGTERM` | 15 | Graceful shutdown request | Yes |
| `SIGKILL` | 9 | Forced immediate termination | **No** |
| `SIGSTOP` | 19 | Pause execution | **No** |
| `SIGCONT` | 18 | Resume paused process | Yes |
| `SIGCHLD` | 17 | Child process state changed | Yes |

> `SIGKILL` and `SIGSTOP` cannot be caught, blocked, or ignored by the target process.

---

## Nice Values & Scheduling

Linux uses the **Completely Fair Scheduler (CFS)**. Priority is influenced by a process's *nice value*:

```
-20  ◄──────────────────────────────────►  +19
 Highest priority               Lowest priority
 (more CPU time)               (less CPU time)
```

- Default nice value: **0**
- Only `root` can set **negative** nice values
- Kernel priority = `20 + nice` (range 0–39)

---

## Requirements

| Component | Requirement |
|---|---|
| OS | Linux (any distro) |
| C compiler | GCC |
| Python | 3.8+ |
| Browser | Any modern browser |

Python packages: `flask`, `flask-cors`, `psutil`

---

## Challenges & Notes

**Zombie processes** occur when a child exits but the parent has not called `waitpid()`. The C demo shows how to prevent this using both `waitpid()` blocking and `SIGCHLD` handlers with `WNOHANG`.

**Permission errors** are expected when sending signals to processes owned by other users (e.g., kernel threads). The API returns a `403` with a descriptive message in these cases.

**`SIGKILL` vs `SIGTERM`:** `SIGTERM` gives the process a chance to clean up; `SIGKILL` cannot be intercepted and forces immediate termination. Always prefer `SIGTERM` first.

**The `/proc` filesystem** is a virtual filesystem — no disk I/O occurs when reading from it. It exposes live kernel data structures as regular files, making it the most direct way to inspect processes from userspace.

---

## References

1. Kerrisk, M. (2010). *The Linux Programming Interface*. No Starch Press.
2. Linux man pages: `fork(2)`, `exec(3)`, `wait(2)`, `kill(2)`, `nice(2)`, `setpriority(2)`, `sigaction(2)`, `proc(5)`
3. Love, R. (2010). *Linux Kernel Development* (3rd ed.). Addison-Wesley.
4. psutil documentation — https://psutil.readthedocs.io
5. Flask documentation — https://flask.palletsprojects.com