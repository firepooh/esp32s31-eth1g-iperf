"""PC-side traffic peer for the board's iperf (iperf 2.x style, TCP/UDP, send/recv).

Use it when iperf 2.x is not installed on the PC. The board's iperf output is the
reference; this peer only generates or sinks traffic and keeps its own byte counts.

Examples (board at 192.168.0.71):
  python netpeer.py tcp-send --host 192.168.0.71                 # board: iperf -s
  python netpeer.py tcp-send --host 192.168.0.71 --streams 2     # board: iperf -s -p 5001 / -p 5002
  python netpeer.py tcp-recv --streams 4                         # board: iperf -c <PC IP> -P 4
  python netpeer.py udp-send --host 192.168.0.71 --rate 150      # board: iperf -u -s
  python netpeer.py udp-recv                                     # board: iperf -u -c <PC IP>
"""
import argparse
import socket
import struct
import threading
import time


def _report(label, t0, start, end, nbytes):
    secs = end - start
    if secs > 0:
        print(f"[{label}] {start - t0:5.1f}-{end - t0:5.1f} s  {nbytes / 1e6:8.2f} MB  "
              f"{nbytes * 8 / secs / 1e6:7.2f} Mbit/s", flush=True)


def _monitor(label, counters, done, t0, interval, quiet):
    """Print the summed per-interval rate of all streams until every stream is done."""
    last, last_total = t0, 0
    while not all(done):
        time.sleep(0.05)
        now = time.perf_counter()
        if now - last >= interval:
            total = sum(counters)
            if not quiet:
                _report(label, t0, last, now, total - last_total)
            last, last_total = now, total


def tcp_send(host, port=5001, streams=1, seconds=10.0, length=131072, interval=1.0, quiet=False):
    """Open `streams` connections to port, port+1, ... and send for `seconds`."""
    counters, done = [0] * streams, [False] * streams
    buf = (bytes(range(256)) * (length // 256 + 1))[:length]
    socks = []
    for i in range(streams):
        s = socket.create_connection((host, port + i), timeout=5)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
        socks.append(s)
    t0 = time.perf_counter()
    end = t0 + seconds

    def worker(i, s):
        try:
            while time.perf_counter() < end:
                counters[i] += s.send(buf)
            s.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        finally:
            s.close()
            done[i] = True

    threads = [threading.Thread(target=worker, args=(i, s)) for i, s in enumerate(socks)]
    for t in threads:
        t.start()
    _monitor("tcp-send", counters, done, t0, interval, quiet)
    for t in threads:
        t.join()
    elapsed = time.perf_counter() - t0
    if not quiet:
        _report(f"tcp-send TOTAL x{streams}", t0, t0, t0 + elapsed, sum(counters))
    return {"bytes": sum(counters), "per_stream": counters, "seconds": elapsed}


def tcp_recv(port=5001, streams=1, wait=30.0, late=15.0, interval=1.0, quiet=False, ready=None):
    """Accept up to `streams` connections and count bytes until all close.

    The first connection may take up to `wait` seconds; each later one up to `late` seconds.
    """
    counters, done = [0] * streams, [False] * streams
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    ls.bind(("0.0.0.0", port))
    ls.listen(streams)
    ls.settimeout(wait)
    if not quiet:
        print(f"[tcp-recv] listening on :{port} for {streams} stream(s)", flush=True)
    if ready:
        ready.set()

    def worker(i, c):
        rbuf = bytearray(1 << 18)
        c.settimeout(5)
        try:
            while True:
                try:
                    got = c.recv_into(rbuf)
                except (socket.timeout, OSError):
                    break
                if not got:
                    break
                counters[i] += got
        finally:
            c.close()
            done[i] = True

    threads = []
    first = threading.Event()

    def acceptor():
        try:
            for i in range(streams):
                c, peer = ls.accept()
                first.set()
                ls.settimeout(late)
                if not quiet:
                    print(f"[tcp-recv] stream {i} from {peer[0]}:{peer[1]}", flush=True)
                t = threading.Thread(target=worker, args=(i, c))
                t.start()
                threads.append(t)
        except socket.timeout:
            if not quiet:
                print(f"[tcp-recv] only {len(threads)} of {streams} streams connected", flush=True)
            for i in range(len(threads), streams):
                done[i] = True
            first.set()

    acc = threading.Thread(target=acceptor, daemon=True)
    acc.start()
    first.wait()
    t0 = time.perf_counter()
    _monitor("tcp-recv", counters, done, t0, interval, quiet)
    acc.join()
    for t in threads:
        t.join()
    ls.close()
    elapsed = time.perf_counter() - t0
    if not quiet:
        _report(f"tcp-recv TOTAL x{len(threads)}", t0, t0, t0 + elapsed, sum(counters))
    return {"bytes": sum(counters), "per_stream": counters, "connected": len(threads), "seconds": elapsed}


def udp_send(host, port=5001, seconds=10.0, rate=100.0, length=1470, interval=1.0, quiet=False):
    """Send numbered datagrams at `rate` Mbit/s (paced) for `seconds`."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    payload = bytearray(length)
    pps = rate * 1e6 / (length * 8)
    t0 = last = time.perf_counter()
    end = t0 + seconds
    seq = chunk = total = errors = 0
    while True:
        now = time.perf_counter()
        if now >= end:
            break
        due = int((now - t0) * pps)
        while seq < due:
            struct.pack_into("!I", payload, 0, seq)
            try:
                s.sendto(payload, (host, port))
                total += length
                chunk += length
            except OSError:
                errors += 1
            seq += 1
        if now - last >= interval:
            if not quiet:
                _report("udp-send", t0, last, now, chunk)
            last, chunk = now, 0
    elapsed = time.perf_counter() - t0
    s.close()
    if not quiet:
        _report("udp-send TOTAL", t0, t0, t0 + elapsed, total)
        print(f"[udp-send] datagrams={seq} len={length} send_errors={errors}", flush=True)
    return {"bytes": total, "datagrams": seq, "errors": errors, "seconds": elapsed}


def udp_recv(port=5001, wait=30.0, idle=2.0, interval=1.0, quiet=False, ready=None):
    """Count datagrams until `idle` seconds pass without one; report loss from packet ids."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    s.bind(("0.0.0.0", port))
    s.settimeout(wait)
    if not quiet:
        print(f"[udp-recv] listening on :{port}", flush=True)
    if ready:
        ready.set()
    d, peer = s.recvfrom(65536)
    s.settimeout(idle)
    rbuf = bytearray(65536)
    unpack = struct.Struct("!I").unpack_from
    t0 = last = time.perf_counter()
    total = chunk = len(d)
    count = 1
    first_id = max_id = unpack(d, 0)[0]
    out_of_order = 0
    while True:
        try:
            n = s.recv_into(rbuf)
        except socket.timeout:
            break
        now = time.perf_counter()
        pid = unpack(rbuf, 0)[0]
        if pid < max_id:
            out_of_order += 1
        elif pid > max_id:
            max_id = pid
        count += 1
        total += n
        chunk += n
        if now - last >= interval:
            if not quiet:
                _report("udp-recv", t0, last, now, chunk)
            last, chunk = now, 0
    s.close()
    elapsed = time.perf_counter() - t0 - idle
    expected = max_id - first_id + 1
    result = {"bytes": total, "datagrams": count, "expected": expected,
              "lost": expected - count, "out_of_order": out_of_order, "seconds": elapsed}
    if not quiet:
        _report("udp-recv TOTAL", t0, t0, t0 + elapsed, total)
        print(f"[udp-recv] received={count} expected={expected} lost={expected - count} "
              f"({100.0 * (expected - count) / expected:.2f}%) out_of_order={out_of_order}", flush=True)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mode", choices=["tcp-send", "tcp-recv", "udp-send", "udp-recv"])
    p.add_argument("--host", help="board IP (send modes)")
    p.add_argument("--port", type=int, default=5001)
    p.add_argument("--streams", type=int, default=1, help="TCP parallel streams")
    p.add_argument("--time", type=float, default=10, help="send duration, s")
    p.add_argument("--interval", type=float, default=1, help="report interval, s")
    p.add_argument("--len", type=int, default=None, help="write size (TCP 131072, UDP 1470)")
    p.add_argument("--rate", type=float, default=100, help="UDP send rate, Mbit/s")
    p.add_argument("--wait", type=float, default=30, help="seconds to wait for the peer")
    a = p.parse_args()
    if a.mode == "tcp-send":
        tcp_send(a.host, a.port, a.streams, a.time, a.len or 131072, a.interval)
    elif a.mode == "tcp-recv":
        tcp_recv(a.port, a.streams, a.wait, interval=a.interval)
    elif a.mode == "udp-send":
        udp_send(a.host, a.port, a.time, a.rate, a.len or 1470, a.interval)
    else:
        udp_recv(a.port, a.wait, interval=a.interval)


if __name__ == "__main__":
    main()
