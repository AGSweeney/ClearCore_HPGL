"""Stand-in for a ClearHPGL_Firmware board, for developing and testing host software.

    python Firmware/host/fake_plotter.py [--rate 20000] [--bind 0.0.0.0]

Speaks the board's network protocol (Firmware/PROTOCOL.md):
  TCP 9300  console: status, config, set, enable, disable, abort, pause, resume,
            origin, goto, pen, clear, version, exit
  TCP 9100  HP-GL jobs, read at --rate bytes/s to imitate the board's back-pressure
  UDP 9302  CLEARHPGL_DISCOVER?

It tracks PU/PD/PA/PR coordinates roughly (absolute plotter units only, no
scaling) so status shows the pen moving. It does not interpret HP-GL fully.
"""

import argparse
import re
import socket
import threading
import time

PU_PER_MM = 40.0


class Plotter:
    def __init__(self):
        self.lock = threading.Lock()
        self.enabled = False
        self.paused = False
        self.fault = "none"
        self.x = 0.0
        self.y = 0.0
        self.pen_down = False
        self.job_active = False
        self.job_bytes = 0
        self.busy_until = 0.0
        self.abort = False
        self.config = {
            "steps_rev_x": "800", "steps_rev_y": "800", "mm_rev_x": "5.000", "mm_rev_y": "5.000",
            "dir_x": "1", "dir_y": "1", "width_mm": "400.000", "height_mm": "300.000",
            "draw_mm_s": "100.000", "travel_mm_s": "200.000", "max_mm_s": "250.000",
            "accel_mm_s2": "2000.000", "corner_mm_s": "20.000", "pen_mode": "0", "pen_io": "0",
            "pen_active_high": "1", "pen_down_ms": "150", "pen_up_ms": "100", "z_up_steps": "0",
            "z_down_steps": "800", "z_vel_steps": "20000", "z_accel_steps": "400000",
            "estop_di6": "0", "auto_enable": "1", "hlfb_check": "1", "com_port": "0",
            "com_baud": "9600", "com_handshake": "1", "com_rs232": "1", "net_mode": "0",
            "ip": "0.0.0.0", "netmask": "0.0.0.0", "gateway": "0.0.0.0", "ident": "7475A",
        }

    def state(self):
        if self.fault != "none":
            return "fault"
        if not self.enabled:
            return "disabled"
        if self.paused:
            return "paused"
        if self.job_active or time.time() < self.busy_until:
            return "plotting"
        return "idle"

    def status_lines(self):
        with self.lock:
            busy = self.state() == "plotting"
            return [
                f"state={self.state()} enabled={int(self.enabled)} x_mm={self.x / PU_PER_MM:.3f} "
                f"y_mm={self.y / PU_PER_MM:.3f} pen={'down' if self.pen_down else 'up'} sel_pen=1 "
                f"queue={40 if busy else 0} planner={8 if busy else 0} segments=0 dropped=0 pages=0 "
                f"estop=ok fault={self.fault}",
                f"source={'tcp' if self.job_active else 'none'} usb_in=0 com=off com_in=0 com_overflow=0 "
                f"tcp_in={self.job_bytes} tcp_jobs=0 tcp_connected={int(self.job_active)} console=1 instr=0 "
                f"unknown=0 labels_skipped=0 hpgl_error=0 ip=127.0.0.1",
            ]

    def command(self, line):
        parts = line.split()
        cmd = parts[0].lower() if parts else ""
        with self.lock:
            if cmd == "status":
                pass
            elif cmd == "config":
                return [f"{k:<16} = {v:<16} (fake)" for k, v in self.config.items()] + ["(stored in NVM)", "ok"]
            elif cmd == "set" and len(parts) >= 3:
                if parts[1] not in self.config:
                    return ["error: unknown key (try: config)"]
                self.config[parts[1]] = parts[2]
                return ["ok"]
            elif cmd == "enable":
                self.fault = "none"
                self.enabled = True
                return ["ok"]
            elif cmd == "disable":
                self.enabled = False
                return ["ok"]
            elif cmd == "abort":
                self.abort = True
                self.pen_down = False
                self.busy_until = 0
                return ["ok"]
            elif cmd in ("pause", "resume"):
                self.paused = cmd == "pause"
                return ["ok"]
            elif cmd == "origin":
                self.x = self.y = 0.0
                return ["ok"]
            elif cmd == "goto" and len(parts) >= 3:
                if self.job_active:
                    return ["error: a job is running"]
                self.enabled = True
                self.pen_down = False
                self.x, self.y = float(parts[1]) * PU_PER_MM, float(parts[2]) * PU_PER_MM
                return ["ok"]
            elif cmd == "pen" and len(parts) >= 2:
                self.enabled = True
                self.pen_down = parts[1] == "down"
                return ["ok"]
            elif cmd == "clear":
                self.fault = "none"
                return ["ok"]
            elif cmd == "version":
                return ["ClearHPGL_Firmware 0.1.0 (fake)", "ok"]
            elif cmd in ("exit", "quit"):
                return ["ok"]
            else:
                return ["error: unknown command (try: help)"]
        return self.status_lines() + ["ok"]


COORD = re.compile(rb"(PU|PD|PA|PR)([-0-9.,\s]*)", re.I)


def track_hpgl(p, data, carry):
    """Very rough pen tracking: absolute coordinates on PU/PD/PA."""
    text = carry + data
    cut = text.rfind(b";")
    if cut < 0:
        return text[-256:]
    for m in COORD.finditer(text[:cut + 1]):
        op = m.group(1).upper()
        nums = [float(n) for n in re.findall(rb"-?\d+(?:\.\d+)?", m.group(2))]
        with p.lock:
            if op == b"PU":
                p.pen_down = False
            elif op == b"PD":
                p.pen_down = True
            if op != b"PR" and len(nums) >= 2:
                p.x, p.y = nums[-2], nums[-1]
    return text[cut + 1:]


def serve_console(p, conn):
    buf = b""
    with conn:
        while True:
            try:
                chunk = conn.recv(1024)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf or b"\r" in buf:
                idx = min(i for i in (buf.find(b"\r"), buf.find(b"\n")) if i >= 0)
                line, buf = buf[:idx].decode(errors="replace").strip(), buf[idx + 1:]
                if not line:
                    continue
                reply = p.command(line)
                conn.sendall(("\r\n".join(reply) + "\r\n").encode())
                if line.split()[0].lower() in ("exit", "quit"):
                    return


def serve_job(p, conn, rate):
    with conn:
        with p.lock:
            p.job_active = True
            p.abort = False
            p.enabled = True
        carry = b""
        conn.settimeout(0.5)
        while True:
            with p.lock:
                if p.abort or p.fault != "none":
                    break
                paused = p.paused
            if paused:
                time.sleep(0.1)
                continue
            try:
                chunk = conn.recv(max(1, rate // 20))
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            with p.lock:
                p.job_bytes += len(chunk)
            carry = track_hpgl(p, chunk, carry)
            time.sleep(0.05)
        with p.lock:
            p.job_active = False
            p.busy_until = 0 if p.abort else time.time() + 1.5  # "plotting from the buffer"


def listen(port, bind, handler):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind, port))
    srv.listen(1)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handler, args=(conn,), daemon=True).start()


def discovery(bind):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((bind, 9302))
    while True:
        data, addr = s.recvfrom(256)
        if data.startswith(b"CLEARHPGL_DISCOVER?"):
            s.sendto(b"CLEARHPGL ClearHPGL_Firmware IP=127.0.0.1 HPGL=9100 CONSOLE=9300 FW=0.1.0-fake "
                     b"IDENT=7475A", addr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--rate", type=int, default=20000, help="job bytes per second the fake board accepts")
    args = ap.parse_args()
    p = Plotter()
    threading.Thread(target=listen, args=(9300, args.bind, lambda c: serve_console(p, c)), daemon=True).start()
    threading.Thread(target=listen, args=(9100, args.bind, lambda c: serve_job(p, c, args.rate)), daemon=True).start()
    threading.Thread(target=discovery, args=(args.bind,), daemon=True).start()
    print(f"fake plotter on {args.bind}: console 9300, HP-GL 9100, discovery 9302 (Ctrl+C to stop)", flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
