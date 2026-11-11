"""Run the iperf benchmark suite against the board and save the results as JSON.

The script drives the board's console over its UART (USB-UART port) and runs the
PC side of every test with netpeer.py, so one command measures everything:

  python tools/bench.py --port COM20 --board 192.168.0.71 --pc 192.168.0.12 \
         --label single-core --out docs/results/1g-single-core.json

Close any serial terminal on the port first. Throughput numbers come from the
board's iperf report; the PC byte counts are kept for cross-checking.
"""
import argparse
import datetime
import json
import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serial  # noqa: E402  (pyserial, ships with ESP-IDF's Python env)
import netpeer  # noqa: E402

REPORT_RE = re.compile(r"\[\s*(\d+)\]\s+([\d.]+)-\s*([\d.]+) sec\s+([\d.]+) MBytes\s+([\d.]+) Mbits/sec")
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


class Console:
    """Minimal expect-style wrapper around the board's esp_console REPL."""

    def __init__(self, port, baud=115200):
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, baud, 0.05
        self.ser.dtr = False  # keep EN/IO0 released: do not reset the board on open
        self.ser.rts = False
        self.ser.open()
        self.buf = ""

    def _pump(self):
        data = self.ser.read(4096)
        if data:
            if b"\x1b[6n" in data:
                # linenoise asks for the cursor position; answer so it does not eat our input
                self.ser.write(b"\x1b[1;1R")
            self.buf += ANSI_RE.sub("", data.decode("utf-8", "replace")).replace("\r", "")

    def reset(self, timeout=25):
        """Reset via the USB-UART auto-reset circuit and wait for DHCP and the prompt.

        Booting with nobody answering the terminal probe puts linenoise in dumb mode,
        which is the most predictable mode for scripted input.
        """
        self.ser.rts = True   # EN low (DTR stays low, so IO0 stays high: normal boot)
        time.sleep(0.1)
        self.ser.rts = False
        start = len(self.buf)
        if not self.wait(r"GOT IP[^\n]*\n", start, timeout):
            raise RuntimeError("board did not get an IP address after reset")
        time.sleep(0.5)

    def mark(self):
        self._pump()
        return len(self.buf)

    def send(self, line):
        time.sleep(0.3)  # let the prompt settle; typing over output garbles the line
        self.ser.write((line + "\r").encode())
        self.ser.flush()

    def wait(self, pattern, start, timeout):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            self._pump()
            m = rx.search(self.buf, start)
            if m:
                return m
            time.sleep(0.02)
        return None

    def wait_reports(self, start, count, min_end, timeout):
        """Wait until `count` summary lines (start 0.0, end >= min_end) appear after `start`."""
        end = time.time() + timeout
        while time.time() < end:
            self._pump()
            if len(self.summaries(start, min_end)) >= count:
                break
            time.sleep(0.05)
        time.sleep(0.3)
        self._pump()
        return self.summaries(start, min_end)

    def summaries(self, start, min_end):
        out = {}
        for m in REPORT_RE.finditer(self.buf, start):
            sid, t0, t1, mb, mbps = int(m[1]), float(m[2]), float(m[3]), float(m[4]), float(m[5])
            if t0 == 0.0 and t1 >= min_end:
                out[sid] = {"seconds": t1, "mbytes": mb, "mbps": mbps}
        return out

    def intervals(self, start, stop=None):
        out = {}
        for m in REPORT_RE.finditer(self.buf, start, stop if stop is not None else len(self.buf)):
            sid, t0, t1, mbps = int(m[1]), float(m[2]), float(m[3]), float(m[5])
            if abs((t1 - t0) - 1.0) < 0.01:
                out.setdefault(sid, []).append({"t0": t0, "t1": t1, "mbps": mbps})
        return out

    def text(self, start, stop=None):
        return self.buf[start:stop]

    def command(self, line, done_pattern, timeout=5):
        start = self.mark()
        self.send(line)
        if not self.wait(done_pattern, start, timeout):
            raise RuntimeError(f"no '{done_pattern}' after '{line}':\n{self.text(start)[-500:]}")
        return start


def stats(values):
    return {"mean": round(sum(values) / len(values), 2), "min": min(values), "max": max(values), "runs": values}


def run_bg(fn, **kw):
    """Run a netpeer receiver in a thread; wait until it listens."""
    ready, box = threading.Event(), {}
    t = threading.Thread(target=lambda: box.update(result=fn(ready=ready, quiet=True, **kw)), daemon=True)
    t.start()
    ready.wait(5)
    return t, box


def median(values):
    s = sorted(values)
    n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def test_tcp_rx(con, a):
    """TCP RX stalls now and then, so keep the per-second trace as well as the 10 s average."""
    runs, medians, details = [], [], []
    for _ in range(a.rx_reps):
        start = con.command("iperf -s -i 1 -t 14", r"Socket created")
        pc = netpeer.tcp_send(a.board, 5001, 1, 10, quiet=True)
        rep = con.wait_reports(start, 1, 5, 20)
        mbps = next(iter(rep.values()))["mbps"]
        ivals = [x["mbps"] for v in con.intervals(start).values() for x in v]
        med = median(ivals)
        runs.append(mbps)
        medians.append(med)
        details.append({"board_mbps": mbps, "per_second_median": med,
                        "stall_seconds": sum(1 for v in ivals if v < 0.5 * med),
                        "per_second": ivals, "pc_sent_mbytes": round(pc["bytes"] / 1e6, 2)})
        time.sleep(1)
    return {"board_mbps": stats(runs), "per_second_median_mbps": stats(medians), "runs": details}


def test_tcp_tx(con, a):
    runs, medians, details = [], [], []
    for _ in range(a.reps):
        t, box = run_bg(netpeer.tcp_recv, port=5001, streams=1, wait=20)
        start = con.mark()
        con.send(f"iperf -c {a.pc} -i 1 -t 10")
        rep = con.wait_reports(start, 1, 9.5, 25)
        t.join(20)
        mbps = next(iter(rep.values()))["mbps"]
        ivals = [x["mbps"] for v in con.intervals(start).values() for x in v]
        med = median(ivals)
        runs.append(mbps)
        medians.append(med)
        details.append({"board_mbps": mbps, "per_second_median": med,
                        "stall_seconds": sum(1 for v in ivals if v < 0.5 * med),
                        "per_second": ivals, "pc_received_mbytes": round(box["result"]["bytes"] / 1e6, 2)})
        time.sleep(1)
    return {"board_mbps": stats(runs), "per_second_median_mbps": stats(medians), "runs": details}


def test_udp_tx(con, a):
    runs, details = [], []
    for _ in range(a.reps):
        t, box = run_bg(netpeer.udp_recv, port=5001, wait=20)
        start = con.mark()
        con.send(f"iperf -u -c {a.pc} -i 1 -t 10")
        rep = con.wait_reports(start, 1, 9.5, 25)
        t.join(20)
        r = next(iter(rep.values()))
        pc = box["result"]
        runs.append(r["mbps"])
        details.append({"board_mbps": r["mbps"], "board_sent_mbytes": r["mbytes"],
                        "pc_received_mbytes": round(pc["bytes"] / 1e6, 2),
                        "pc_lost_by_packet_id": pc["lost"], "pc_datagrams": pc["datagrams"]})
        time.sleep(1)
    return {"board_mbps": stats(runs), "runs": details}


def test_udp_rx_sweep(con, a):
    points = []
    for rate in a.rates:
        start = con.command("iperf -u -s -i 1 -t 8", r"Socket created")
        time.sleep(0.3)
        pc = netpeer.udp_send(a.board, 5001, 5, rate, quiet=True)
        rep = con.wait_reports(start, 1, 7.5, 15)
        r = next(iter(rep.values()))
        received = r["mbytes"] * 1e6
        points.append({
            "offered_mbps": rate,
            "pc_sent_mbytes": round(pc["bytes"] / 1e6, 2),
            "board_received_mbytes": r["mbytes"],
            "received_mbps": round(received * 8 / pc["seconds"] / 1e6, 1),
            "loss_pct": round(max(0.0, 100.0 * (1 - received / pc["bytes"])), 2),
            "starving_warnings": con.text(start).count("starving"),
        })
        time.sleep(2)  # let the RX path recover after an overload
    return points


def test_tcp_tx_multi(con, a, streams=4):
    t, box = run_bg(netpeer.tcp_recv, port=5001, streams=streams, wait=20, late=3)
    start = con.mark()
    con.send(f"iperf -c {a.pc} -P {streams} -i 1 -t 10")
    rep = con.wait_reports(start, streams, 9.5, 30)
    t.join(30)
    con.command("iperf --abort", r"\niperf> ", 5)  # stop streams that never got to run
    pc = box["result"]
    totals = per_second_totals(con.intervals(start))
    return {"requested_streams": streams, "connected_streams": pc["connected"],
            "board_per_stream_mbps": [v["mbps"] for v in rep.values()],
            "board_total_mbps": round(sum(v["mbps"] for v in rep.values()), 2),
            "per_second_total": totals,
            "per_second_median_mbps": round(median(totals), 1) if totals else None,
            "pc_total_mbps": round(pc["bytes"] * 8 / 10 / 1e6, 2)}


def per_second_totals(ivals):
    sums = {}
    for v in ivals.values():
        for x in v:
            sums[x["t0"]] = sums.get(x["t0"], 0.0) + x["mbps"]
    return [round(sums[k], 1) for k in sorted(sums)]


def test_tcp_rx_multi(con, a, streams=2):
    start = None
    for i in range(streams):
        s = con.command(f"iperf -s -p {5001 + i} -i 1 -t 18", r"Socket created")
        start = s if start is None else start
    netpeer.tcp_send(a.board, 5001, streams, 10, quiet=True)
    rep = con.wait_reports(start, streams, 5, 25)
    ivals = con.intervals(start)
    steady = {}
    for v in ivals.values():
        for x in v:
            if x["t0"] >= 4.0:
                steady[x["t0"]] = steady.get(x["t0"], 0.0) + x["mbps"]
    totals = per_second_totals(ivals)
    return {"streams": streams,
            "board_per_stream_mbps": [v["mbps"] for v in rep.values()],
            "board_total_mbps": round(sum(v["mbps"] for v in rep.values()), 2),
            "per_second_total": totals,
            "per_second_median_mbps": round(median(totals), 1) if totals else None,
            "steady_total_mbps_4_to_10s": round(sum(steady.values()) / len(steady), 1) if steady else None}


def phy_snapshot(con):
    start = con.command("ethernet phy", r"LED cfg[^\n]*\n", 5)
    txt = con.text(start)
    m = re.search(r"idle error count = (\d+)", txt)
    g = re.search(r"GBSR\s*:\s*partner[^\n]*", txt)
    s = re.search(r"SSR\s*:\s*[^\n]*", txt)
    return {"idle_errors": int(m[1]) if m else None, "gbsr": g[0] if g else None, "ssr": s[0] if s else None}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", default="COM20")
    p.add_argument("--board", default="192.168.0.71")
    p.add_argument("--pc", default="192.168.0.12")
    p.add_argument("--label", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--reps", type=int, default=3, help="runs per TX test")
    p.add_argument("--rx-reps", type=int, default=5, help="runs for single-stream TCP RX (it varies most)")
    p.add_argument("--rates", type=float, nargs="+", default=[100, 125, 150, 175, 200, 250, 300])
    p.add_argument("--only", nargs="+", help="re-run only these tests and merge them into --out")
    p.add_argument("--flowctrl", choices=["default", "on", "off"], default="default",
                   help="pause-frame flow control during the test (default: firmware's boot setting)")
    a = p.parse_args()

    con = Console(a.port)
    con.reset()
    con.command("", r"iperf> ", 5)
    if a.flowctrl != "default":
        start = con.command(f"ethernet flowctrl {a.flowctrl}", r"iperf> ", 5)
        if not con.wait(r"LINK UP[^\n]*\n", start, 20):
            raise RuntimeError("link did not come back after changing flow control")
        time.sleep(4)
    start = con.command("ethernet stats", r"Flow control:[^\n]*\n", 5)
    flow_state = re.search(r"Flow control:\s*([^\n]*)", con.text(start))[1]
    start = con.command("ethernet info", r"IP\s+:[^\n]*\n", 5)
    info = con.text(start)
    result = {}
    if a.only and os.path.exists(a.out):
        with open(a.out, encoding="utf-8") as f:
            result = json.load(f)
    result.update({"label": a.label, "date": datetime.datetime.now().isoformat(timespec="seconds"),
                   "board_ip": a.board, "pc_ip": a.pc, "flow_control": flow_state,
                   "link": re.search(r"MAC speed\s*:\s*([^\n]*)", info)[1]})
    # warm-up (not recorded): the first transfer after boot can stall on ARP/TCP start-up
    start = con.command("iperf -s -i 1 -t 8", r"Socket created")
    netpeer.tcp_send(a.board, 5001, 1, 4, quiet=True)
    con.wait_reports(start, 1, 3, 15)
    time.sleep(1)
    steps = [("tcp_rx", test_tcp_rx), ("tcp_tx", test_tcp_tx), ("udp_tx", test_udp_tx),
             ("udp_rx_sweep", test_udp_rx_sweep), ("tcp_tx_multi", test_tcp_tx_multi),
             ("tcp_rx_multi", test_tcp_rx_multi)]
    for name, fn in steps:
        if a.only and name not in a.only:
            continue
        print(f"== {name}", flush=True)
        try:
            result[name] = fn(con, a)
        except Exception as e:  # keep going; record what failed
            result[name] = {"error": repr(e)}
            con.command("iperf --abort", r"\niperf> ", 5)
        print(json.dumps(result[name]), flush=True)
    result["phy_after"] = phy_snapshot(con)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    print(f"saved {a.out}", flush=True)


if __name__ == "__main__":
    main()
