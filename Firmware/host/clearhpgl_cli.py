"""Bench client for ClearHPGL_Firmware. Standard library only (pyserial for COM ports).

    python Firmware/host/clearhpgl_cli.py discover
    python Firmware/host/clearhpgl_cli.py --host 192.168.0.110 send drawing.plt
    python Firmware/host/clearhpgl_cli.py --serial COM5 send drawing.plt
    python Firmware/host/clearhpgl_cli.py --host 192.168.0.110 query "OI;OS;OA;"
    python Firmware/host/clearhpgl_cli.py --host 192.168.0.110 console status
    python Firmware/host/clearhpgl_cli.py --host 192.168.0.110 console set draw_mm_s 80

`send` over TCP streams the file to port 9100 and closes (one connection = one job).
Over a serial port it sends with XON/XOFF, the way a plotter driver would.
"""

import argparse
import socket
import sys
import time

HPGL_PORT = 9100
CONSOLE_PORT = 9300
DISCOVERY_PORT = 9302
DISCOVERY_REQUEST = b"CLEARHPGL_DISCOVER?"


def discover(timeout):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(timeout)
    s.sendto(DISCOVERY_REQUEST, ("255.255.255.255", DISCOVERY_PORT))
    found = 0
    end = time.time() + timeout
    while time.time() < end:
        try:
            data, addr = s.recvfrom(512)
        except socket.timeout:
            break
        print(f"{addr[0]}: {data.decode(errors='replace')}")
        found += 1
    if not found:
        print("no plotters answered")
    return 0 if found else 1


def read_replies(sock_or_serial, count, timeout, is_serial=False):
    """HP-GL replies end with CR (ESC.M default)."""
    buf = b""
    replies = []
    end = time.time() + timeout
    while len(replies) < count and time.time() < end:
        if is_serial:
            chunk = sock_or_serial.read(64)
        else:
            try:
                chunk = sock_or_serial.recv(256)
            except socket.timeout:
                break
            if not chunk:
                break
        buf += chunk
        while b"\r" in buf:
            line, buf = buf.split(b"\r", 1)
            line = line.strip(b"\n\x11\x13")
            replies.append(line.decode(errors="replace"))
    return replies


def output_instruction_count(hpgl):
    text = hpgl.upper()
    return sum(text.count(m) for m in ("OI", "OS", "OA", "OC", "OE", "OP", "OH", "OW", "OF", "OO", "OT", "OD"))


def open_serial(port, baud):
    try:
        import serial  # type: ignore
    except ImportError:
        sys.exit("pyserial is required for --serial (pip install pyserial)")
    return serial.Serial(port, baud, timeout=0.2, xonxoff=True)


def cmd_send(args):
    data = open(args.file, "rb").read()
    t0 = time.time()
    if args.serial:
        ser = open_serial(args.serial, args.baud)
        for i in range(0, len(data), 64):
            ser.write(data[i:i + 64])
        ser.flush()
        ser.close()
    else:
        with socket.create_connection((args.host, HPGL_PORT), timeout=10) as s:
            s.settimeout(None)
            s.sendall(data)
            s.shutdown(socket.SHUT_WR)
            # Drain anything the job asked for (output instructions) until the board closes.
            s.settimeout(2.0)
            try:
                while s.recv(256):
                    pass
            except (socket.timeout, ConnectionResetError):
                pass
    print(f"sent {len(data)} bytes in {time.time() - t0:.1f} s (plotting continues from the board buffer)")
    return 0


def cmd_query(args):
    hpgl = args.hpgl.encode()
    n = output_instruction_count(args.hpgl)
    if args.serial:
        ser = open_serial(args.serial, args.baud)
        ser.write(hpgl)
        replies = read_replies(ser, n, args.timeout, is_serial=True)
        ser.close()
    else:
        with socket.create_connection((args.host, HPGL_PORT), timeout=10) as s:
            s.settimeout(args.timeout)
            s.sendall(hpgl)
            replies = read_replies(s, n, args.timeout)
    for r in replies:
        print(r)
    return 0 if len(replies) == n else 1


def cmd_console(args):
    line = " ".join(args.command)
    with socket.create_connection((args.host, CONSOLE_PORT), timeout=10) as s:
        s.settimeout(args.timeout)
        s.sendall(line.encode() + b"\r\n")
        buf = b""
        while True:
            try:
                chunk = s.recv(1024)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            text = buf.decode(errors="replace")
            last = text.rstrip("\r\n").split("\r\n")[-1]
            if last.startswith("ok") or last.startswith("error:"):
                break
    out = buf.decode(errors="replace").rstrip()
    print(out)
    return 0 if out.split("\r\n")[-1].startswith("ok") else 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", help="board IP (TCP 9100 / 9300)")
    p.add_argument("--serial", help="serial port instead of TCP (USB CDC or COM-0/1)")
    p.add_argument("--baud", type=int, default=9600)
    p.add_argument("--timeout", type=float, default=3.0)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("discover")
    s = sub.add_parser("send")
    s.add_argument("file")
    q = sub.add_parser("query")
    q.add_argument("hpgl", help='e.g. "OI;OS;OA;"')
    c = sub.add_parser("console")
    c.add_argument("command", nargs="+")
    args = p.parse_args()

    if args.cmd == "discover":
        return discover(args.timeout)
    if args.cmd in ("send", "query") and not (args.host or args.serial):
        p.error("--host or --serial is required")
    if args.cmd == "console" and not args.host:
        p.error("--host is required (the USB console is interactive: type 'menu' in a terminal)")
    return {"send": cmd_send, "query": cmd_query, "console": cmd_console}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
