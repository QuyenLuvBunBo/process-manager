"""
server.py — Linux Process Manager REST API
Uses psutil + Linux /proc filesystem + subprocess to manage processes.

Endpoints:
  GET  /api/processes            — List all processes
  GET  /api/processes/<pid>      — Single process details
  GET  /api/processes/<pid>/children — Child processes
  POST /api/processes/create     — Fork and run a command
  POST /api/processes/<pid>/kill — Send signal to process
  POST /api/processes/<pid>/priority — Change nice value
  GET  /api/system               — CPU, memory, load averages
  GET  /api/tree                 — Process tree (JSON)
"""

import os
import sys
import signal
import subprocess
import json
import time
import psutil
from flask import Flask, request, jsonify
from flask_cors import CORS

app = Flask(__name__)
CORS(app)

# ── Helpers ──────────────────────────────────────────────────────────────────

SIGNAL_MAP = {
    "SIGTERM": signal.SIGTERM,
    "SIGKILL": signal.SIGKILL,
    "SIGSTOP": signal.SIGSTOP,
    "SIGCONT": signal.SIGCONT,
    "SIGHUP":  signal.SIGHUP,
    "SIGINT":  signal.SIGINT,
}

STATE_LABELS = {
    'R': 'Running',
    'S': 'Sleeping',
    'D': 'Waiting',
    'Z': 'Zombie',
    'T': 'Stopped',
    'I': 'Idle',
    'X': 'Dead',
}

def process_to_dict(proc):
    """Convert a psutil.Process to a serializable dictionary."""
    try:
        with proc.oneshot():
            info = {
                "pid":        proc.pid,
                "ppid":       proc.ppid(),
                "name":       proc.name(),
                "status":     proc.status(),
                "state_label": STATE_LABELS.get(proc.status()[0].upper(), proc.status()),
                "cpu_percent": proc.cpu_percent(interval=None),
                "memory_mb":  round(proc.memory_info().rss / 1024 / 1024, 2),
                "nice":       proc.nice(),
                "num_threads": proc.num_threads(),
                "create_time": proc.create_time(),
                "username":   proc.username(),
            }
            try:
                info["cmdline"] = " ".join(proc.cmdline()) or proc.name()
            except Exception:
                info["cmdline"] = proc.name()
            return info
    except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
        return None


def read_proc_stat_raw(pid):
    """Read raw /proc/<pid>/stat — demonstrating /proc filesystem access."""
    try:
        with open(f"/proc/{pid}/stat", "r") as f:
            return f.read().strip()
    except Exception:
        return None


def read_loadavg():
    """Read /proc/loadavg directly (Linux API)."""
    try:
        with open("/proc/loadavg", "r") as f:
            parts = f.read().split()
            return {
                "1min":  float(parts[0]),
                "5min":  float(parts[1]),
                "15min": float(parts[2]),
                "running_threads": parts[3],
            }
    except Exception:
        return {}


def read_meminfo():
    """Read /proc/meminfo directly (Linux API)."""
    data = {}
    try:
        with open("/proc/meminfo", "r") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2:
                    key = parts[0].rstrip(':')
                    val = int(parts[1])
                    data[key] = val
    except Exception:
        pass
    return data


# ── Process Routes ────────────────────────────────────────────────────────────

@app.route("/api/processes", methods=["GET"])
def list_processes():
    """Return all running processes with key metrics."""
    sort_by = request.args.get("sort", "pid")
    filter_name = request.args.get("filter", "").lower()

    # Warm up CPU percentages
    for proc in psutil.process_iter():
        try:
            proc.cpu_percent(interval=None)
        except Exception:
            pass

    time.sleep(0.1)  # brief pause for CPU sampling

    processes = []
    for proc in psutil.process_iter():
        d = process_to_dict(proc)
        if d is None:
            continue
        if filter_name and filter_name not in d["name"].lower():
            continue
        processes.append(d)

    # Sort
    reverse = sort_by.startswith("-")
    key = sort_by.lstrip("-")
    if key in ("cpu_percent", "memory_mb", "pid", "nice", "num_threads"):
        processes.sort(key=lambda x: x.get(key, 0), reverse=reverse)
    elif key == "name":
        processes.sort(key=lambda x: x.get("name", "").lower(), reverse=reverse)

    return jsonify({
        "count": len(processes),
        "processes": processes
    })


@app.route("/api/processes/<int:pid>", methods=["GET"])
def get_process(pid):
    """Get detailed info about a single process, including raw /proc stat."""
    try:
        proc = psutil.Process(pid)
        d = process_to_dict(proc)
        if d is None:
            return jsonify({"error": "Process not found"}), 404

        # Enrich with raw /proc data
        d["proc_stat_raw"] = read_proc_stat_raw(pid)

        try:
            d["open_files"] = [f.path for f in proc.open_files()][:10]
        except Exception:
            d["open_files"] = []

        try:
            d["connections"] = len(proc.connections())
        except Exception:
            d["connections"] = 0

        return jsonify(d)
    except psutil.NoSuchProcess:
        return jsonify({"error": f"PID {pid} not found"}), 404


@app.route("/api/processes/<int:pid>/children", methods=["GET"])
def get_children(pid):
    """Get immediate children of a process."""
    try:
        proc = psutil.Process(pid)
        children = []
        for child in proc.children(recursive=False):
            d = process_to_dict(child)
            if d:
                children.append(d)
        return jsonify({"pid": pid, "children": children})
    except psutil.NoSuchProcess:
        return jsonify({"error": f"PID {pid} not found"}), 404


@app.route("/api/processes/create", methods=["POST"])
def create_process():
    """
    Fork + exec a new process using subprocess (wraps fork/exec).
    Body: { "command": "sleep 10", "background": true }
    """
    data = request.get_json() or {}
    command = data.get("command", "").strip()

    if not command:
        return jsonify({"error": "No command provided"}), 400

    # Whitelist safe demo commands
    ALLOWED_PREFIXES = ["sleep", "echo", "ls", "pwd", "date", "whoami",
                        "cat /proc/version", "uname", "uptime", "hostname"]
    is_safe = any(command.startswith(p) for p in ALLOWED_PREFIXES)
    if not is_safe:
        return jsonify({"error": "Command not in allowed list for safety", 
                        "allowed": ALLOWED_PREFIXES}), 403

    try:
        # os.fork() + exec via subprocess
        # Demonstrating: background process creation
        proc = subprocess.Popen(
            command.split(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True   # setsid() — new session
        )

        return jsonify({
            "success": True,
            "pid": proc.pid,
            "command": command,
            "message": f"Process created with PID {proc.pid}"
        })
    except FileNotFoundError:
        return jsonify({"error": f"Command not found: {command.split()[0]}"}), 400
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@app.route("/api/processes/<int:pid>/kill", methods=["POST"])
def kill_process(pid):
    """
    Send a signal to a process.
    Body: { "signal": "SIGTERM" }  (default SIGTERM)
    """
    data = request.get_json() or {}
    sig_name = data.get("signal", "SIGTERM").upper()
    sig = SIGNAL_MAP.get(sig_name)

    if sig is None:
        return jsonify({"error": f"Unknown signal: {sig_name}",
                        "valid": list(SIGNAL_MAP.keys())}), 400

    # Safety: never kill critical system processes
    PROTECTED_PIDS = {1, os.getpid()}
    if pid in PROTECTED_PIDS:
        return jsonify({"error": "Cannot kill protected process"}), 403

    try:
        proc = psutil.Process(pid)
        proc_name = proc.name()

        os.kill(pid, sig)  # ← Linux API: kill(2) syscall

        return jsonify({
            "success": True,
            "pid": pid,
            "name": proc_name,
            "signal": sig_name,
            "message": f"Sent {sig_name} to {proc_name} (PID {pid})"
        })
    except psutil.NoSuchProcess:
        return jsonify({"error": f"PID {pid} not found"}), 404
    except PermissionError:
        return jsonify({"error": "Permission denied (need root for this process)"}), 403


@app.route("/api/processes/<int:pid>/priority", methods=["POST"])
def set_priority(pid):
    """
    Change process scheduling priority (nice value).
    Body: { "nice": 10 }   range: -20 (high) to +19 (low)
    """
    data = request.get_json() or {}
    nice_val = data.get("nice")

    if nice_val is None:
        return jsonify({"error": "Missing 'nice' value"}), 400
    if not isinstance(nice_val, int) or not (-20 <= nice_val <= 19):
        return jsonify({"error": "Nice value must be integer between -20 and 19"}), 400

    try:
        proc = psutil.Process(pid)
        old_nice = proc.nice()
        proc.nice(nice_val)  # ← wraps setpriority(2) syscall
        new_nice = proc.nice()

        return jsonify({
            "success": True,
            "pid": pid,
            "name": proc.name(),
            "old_nice": old_nice,
            "new_nice": new_nice,
            "message": f"Priority changed from {old_nice} to {new_nice}"
        })
    except psutil.NoSuchProcess:
        return jsonify({"error": f"PID {pid} not found"}), 404
    except PermissionError:
        return jsonify({"error": "Permission denied (need root for negative nice)"}), 403


# ── System Routes ─────────────────────────────────────────────────────────────

@app.route("/api/system", methods=["GET"])
def system_info():
    """
    Returns overall system stats.
    Reads directly from /proc filesystem where possible.
    """
    meminfo = read_meminfo()
    loadavg = read_loadavg()

    cpu_times = psutil.cpu_times_percent(interval=0.1)
    mem = psutil.virtual_memory()
    swap = psutil.swap_memory()

    # Count processes by state
    state_counts = {}
    for proc in psutil.process_iter(['status']):
        try:
            s = proc.info['status']
            state_counts[s] = state_counts.get(s, 0) + 1
        except Exception:
            pass

    return jsonify({
        "cpu": {
            "percent": psutil.cpu_percent(interval=0.1),
            "count_logical": psutil.cpu_count(logical=True),
            "count_physical": psutil.cpu_count(logical=False),
            "user":   round(cpu_times.user, 1),
            "system": round(cpu_times.system, 1),
            "idle":   round(cpu_times.idle, 1),
        },
        "memory": {
            "total_mb":     round(mem.total / 1024**2, 1),
            "used_mb":      round(mem.used  / 1024**2, 1),
            "free_mb":      round(mem.free  / 1024**2, 1),
            "available_mb": round(mem.available / 1024**2, 1),
            "percent":      mem.percent,
            "cached_mb":    round(meminfo.get('Cached', 0) / 1024, 1),
            "buffers_mb":   round(meminfo.get('Buffers', 0) / 1024, 1),
        },
        "swap": {
            "total_mb": round(swap.total / 1024**2, 1),
            "used_mb":  round(swap.used  / 1024**2, 1),
            "percent":  swap.percent,
        },
        "load_average": loadavg,
        "process_states": state_counts,
        "total_processes": sum(state_counts.values()),
        "boot_time": psutil.boot_time(),
        "server_pid": os.getpid(),
    })


@app.route("/api/tree", methods=["GET"])
def process_tree():
    """Build a hierarchical process tree starting from PID 1."""
    root_pid = int(request.args.get("root", 1))

    def build_tree(pid, depth=0, max_depth=4):
        if depth > max_depth:
            return None
        try:
            proc = psutil.Process(pid)
            node = {
                "pid":    pid,
                "name":   proc.name(),
                "status": proc.status(),
                "nice":   proc.nice(),
                "children": []
            }
            for child in proc.children(recursive=False):
                child_node = build_tree(child.pid, depth + 1, max_depth)
                if child_node:
                    node["children"].append(child_node)
            return node
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            return None

    tree = build_tree(root_pid)
    return jsonify({"tree": tree})


@app.route("/api/demo/lifecycle", methods=["POST"])
def demo_lifecycle():
    """
    Demonstrates the full process lifecycle via the compiled C program.
    Runs ./process_manager demo and streams the output.
    """
    binary = os.path.join(os.path.dirname(__file__), "process_manager")
    if not os.path.exists(binary):
        return jsonify({"error": "C binary not compiled. Run: gcc process_manager.c -o process_manager"}), 500

    try:
        result = subprocess.run(
            [binary, "demo"],
            capture_output=True, text=True, timeout=15
        )
        # Strip ANSI color codes for JSON
        import re
        ansi_escape = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])')
        output = ansi_escape.sub('', result.stdout)
        return jsonify({
            "output": output,
            "returncode": result.returncode
        })
    except subprocess.TimeoutExpired:
        return jsonify({"error": "Demo timed out"}), 500


# ── Health Check ──────────────────────────────────────────────────────────────

@app.route("/api/health", methods=["GET"])
def health():
    return jsonify({"status": "ok", "pid": os.getpid(), "time": time.time()})


if __name__ == "__main__":
    print(f"[*] Linux Process Manager API — PID {os.getpid()}")
    print(f"[*] Server running on http://localhost:5000")
    print(f"[*] Using Linux /proc filesystem & psutil")
    app.run(host="0.0.0.0", port=5000, debug=False)