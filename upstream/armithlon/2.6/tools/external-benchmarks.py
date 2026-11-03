#!/usr/bin/env python3
"""ARMITHLON external benchmark adapters (MIT). No third-party binaries bundled.

The third-party executables are optional, separately installed by the user.
Scores from glmark2, vkmark and Geekbench are *not* comparable with each other
or with QRX's internal FPS. A 68k guest is not being benchmarked by these tools.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import html
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
from typing import Optional

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REPORT = ROOT / "reports" / "latest"
THIRD_PARTY = ROOT / "data" / "third_party"
GEEK_URL = "https://www.geekbench.com/download/mac/"
SOURCES = {
    "glmark2": {"url": "https://github.com/glmark2/glmark2.git", "branch": "master",
                "license": "GPL-3.0", "summary": "OpenGL ES 2.0 / desktop OpenGL, Linux X11/Wayland"},
    "vkmark": {"url": "https://github.com/vkmark/vkmark.git", "branch": "master",
               "license": "LGPL-2.1", "summary": "Vulkan, Linux X11/Wayland/KMS"},
}


def now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def host_info() -> dict:
    return {"system": platform.system(), "machine": platform.machine(),
            "release": platform.release(), "python": platform.python_version(),
            "host_name": platform.node(), "collected_utc": now()}


def run_command(cmd: list[str], timeout: int = 600, cwd: Optional[Path] = None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors="replace",
                          timeout=timeout, check=False)


def find_binary(name: str) -> Optional[Path]:
    env_name = "ARMITHLON_" + name.upper() + "_BIN"
    custom = os.environ.get(env_name)
    if custom:
        path = Path(custom).expanduser().resolve()
        return path if path.is_file() and os.access(path, os.X_OK) else None
    path = shutil.which(name)
    if path:
        return Path(path)
    build = THIRD_PARTY / name / "build"
    for b in [build / "src" / name, build / "src" / f"{name}-x11", build / name]:
        if b.is_file() and os.access(b, os.X_OK):
            return b
    return None


def parse_score(tool: str, text: str) -> Optional[int]:
    """Use the published overall score only; scene FPS must not be misreported."""
    pattern = rf"(?im)^\s*(?:\[?{re.escape(tool)}\]?\s*)?score\s*:\s*(\d{{1,10}})\s*$"
    all_scores = re.findall(pattern, text)
    return int(all_scores[-1]) if all_scores else None


def read_json(path: Path, default):
    if not path.is_file():
        return default
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (ValueError, OSError):
        raise ValueError(f"Cannot decode existing JSON; refusing to overwrite: {path}")


def atomic_json(path: Path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temp.replace(path)


def new_entry(tool: str, status: str, reason: str = "", **kw) -> dict:
    return {"tool": tool, "status": status, "recorded_utc": now(),
            "scope": "native_host_only", "reason": reason, **kw}


def source_sha(name: str) -> Optional[str]:
    path = THIRD_PARTY / name
    if not (path / ".git").is_dir():
        return None
    p = run_command(["git", "rev-parse", "HEAD"], cwd=path, timeout=15)
    return p.stdout.strip() if p.returncode == 0 else None


def status() -> int:
    print("ARMITHLON 1.1 – externes Benchmark-Inventar")
    print("Host:", platform.system(), platform.machine())
    for tool, info in SOURCES.items():
        binary = find_binary(tool)
        sha = source_sha(tool)
        print(f"{tool:10} source={sha or 'NOT FETCHED'} binary={binary or 'NOT INSTALLED'}")
        if platform.system() == "Darwin":
            print(f"  Hinweis: offizielles {tool} Linux-Fenstersystem; kein nativer macOS-Port mitgeliefert.")
    print("Geekbench 7: proprietär, nicht im ZIP; GUI separat von", GEEK_URL)
    if platform.system() == "Darwin":
        apps = [Path("/Applications/Geekbench 7.app"), Path.home() / "Applications/Geekbench 7.app"]
        print("  GUI:", next((str(p) for p in apps if p.exists()), "NOT FOUND in standard app folders"))
    print("Geekbench 7 Pro erforderlich für offizielle CLI-Automation; GUI-Ergebnisse können importiert werden.")
    return 0


def fetch(names: list[str]) -> int:
    if not shutil.which("git"):
        print("git not found", file=sys.stderr)
        return 2
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    lock = read_json(THIRD_PARTY / "sources.lock.json", {"schema_version": 1, "tools": {}})
    ok = True
    for name in names:
        meta = SOURCES[name]
        dest = THIRD_PARTY / name
        if dest.exists():
            if not (dest / ".git").is_dir():
                print(f"ERROR {name}: directory exists but isn't a Git checkout: {dest}", file=sys.stderr)
                ok = False
                continue
            # Deliberately never auto-update (reproducibility, supply-chain safety).
            print(f"EXISTS {name} {source_sha(name)} (no automatic update)")
        else:
            print(f"FETCH {name}: {meta['url']}")
            try:
                p = run_command(["git", "clone", "--depth", "1", "--branch", meta["branch"],
                                 meta["url"], str(dest)], timeout=180)
            except subprocess.TimeoutExpired:
                print(f"ERROR {name}: clone timed out", file=sys.stderr)
                ok = False
                continue
            if p.returncode:
                print(f"ERROR {name}: {p.stderr[-1500:]}", file=sys.stderr)
                ok = False
                continue
        sha = source_sha(name)
        if sha:
            lock["tools"][name] = {"url": meta["url"], "commit": sha,
                                   "license": meta["license"], "recorded_utc": now()}
    atomic_json(THIRD_PARTY / "sources.lock.json", lock)
    return 0 if ok else 1


def build(names: list[str]) -> int:
    if platform.system() != "Linux":
        print("glmark2/vkmark ship Linux X11/Wayland/KMS backends, not a native macOS M1 backend.", file=sys.stderr)
        print("Use the built-in Metal/Vulkan benchmark on macOS; build third-party tools on Linux ARM64.", file=sys.stderr)
        return 2
    for exe in ["meson", "ninja"]:
        if not shutil.which(exe):
            print(f"Missing {exe}; install via your package manager", file=sys.stderr)
            return 2
    ok = True
    lock = read_json(THIRD_PARTY / "sources.lock.json", {"tools": {}})
    for name in names:
        src = THIRD_PARTY / name
        if not (src / ".git").is_dir():
            print(f"ERROR: run benchmark-tools fetch {name} first", file=sys.stderr)
            ok = False
            continue
        locked_commit = lock.get("tools", {}).get(name, {}).get("commit")
        current_commit = source_sha(name)
        if not locked_commit or locked_commit != current_commit:
            print(f"ERROR {name}: source revision differs from fetch lock; verify/re-fetch intentionally", file=sys.stderr)
            ok = False
            continue
        builddir = src / "build"
        # x11-gl is an official flavor; explicit to avoid compiling unavailable backends.
        flags = ["-Dbuildtype=release"]
        if name == "glmark2":
            flags += ["-Dflavors=x11-gl"]
        cmd = ["meson", "setup", str(builddir), *flags] if not builddir.exists() else ["meson", "setup", "--reconfigure", str(builddir), *flags]
        try:
            setup = run_command(cmd, timeout=240, cwd=src)
            if setup.returncode:
                raise RuntimeError(setup.stderr[-2000:] + "\n" + setup.stdout[-800:])
            res = run_command(["ninja", "-C", str(builddir)], timeout=600)
            if res.returncode:
                raise RuntimeError(res.stderr[-2000:] + "\n" + res.stdout[-800:])
        except (RuntimeError, subprocess.TimeoutExpired) as exc:
            print(f"ERROR {name} build: {exc}", file=sys.stderr)
            ok = False
            continue
        print(f"BUILD OK {name} => {find_binary(name) or builddir}")
    return 0 if ok else 1


def run_tool(name: str, out: Path, timeout: int = 480) -> dict:
    binpath = find_binary(name)
    if binpath is None:
        return new_entry(name, "SKIP", "Not installed/built; use benchmark-tools fetch/build (Linux), or your package manager")
    # Do not misrepresent host GPU tests as emulated QRX86 or 68k work.
    cmd = [str(binpath), "--size", "800x450"]
    try:
        p = run_command(cmd, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return new_entry(name, "ERROR", f"Execution failed: {exc}")
    out.mkdir(parents=True, exist_ok=True)
    log = out / f"{name}.log"
    log.write_text("COMMAND: " + " ".join(cmd) + "\n\nSTDOUT:\n" + p.stdout +
                   "\nSTDERR:\n" + p.stderr, encoding="utf-8")
    if p.returncode:
        return new_entry(name, "ERROR", f"Exit code {p.returncode}, see {log}",
                         exit_code=p.returncode, binary=str(binpath), log=str(log))
    score = parse_score(name, p.stdout + "\n" + p.stderr)
    if score is None:
        return new_entry(name, "UNVERIFIED", "Exit code 0 but no overall Score line; check raw log",
                         binary=str(binpath), log=str(log))
    return new_entry(name, "PASS", "Overall score from native executable stdout",
                     score=score, score_unit="tool-specific-points", binary=str(binpath), log=str(log),
                     binary_sha256=hashlib.sha256(binpath.read_bytes()).hexdigest(),
                     source_commit=source_sha(name))


def base_report(out: Path) -> dict:
    path = out / "external-results.json"
    return read_json(path, {"schema_version": 1, "benchmark_suite": "armithlon-external-v1",
                            "machine": host_info(), "results": {}})


def store(out: Path, entry: dict):
    data = base_report(out)
    data["results"][entry["tool"]] = entry
    data["updated_utc"] = now()
    atomic_json(out / "external-results.json", data)
    report(data, out)


def report(data: dict, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    lines = ["# ARMITHLON 1.1 – External Benchmark Report", "",
             f"Host: {data['machine']['system']} {data['machine']['machine']} | {data.get('updated_utc', '-')}", "",
             "| Tool | Status | Score | Bedeutung |", "|---|---|---:|---|"]
    meta = {"glmark2": "native OpenGL (Linux); eigener Tool-Score",
            "vkmark": "native Vulkan (Linux); eigener Tool-Score",
            "geekbench7": "native CPU / GPU Compute, keine 3D-FPS"}
    for k in ["glmark2", "vkmark", "geekbench7"]:
        entry = data["results"].get(k, {})
        score = str(entry.get("score", "—"))
        if k == "geekbench7" and entry.get("metrics"):
            score = ", ".join(f"{p}: {v}" for p, v in entry["metrics"].items())
        lines.append(f"| {k} | {entry.get('status', 'NOT RUN')} | {score} | {meta[k]} |")
    lines += ["", "**Nicht vergleichbar:** Die Punktzahlen der Tools sowie ARMITHLON-CPU-, GPU- und Encoder-FPS messen unterschiedliche Lasten.",
              "Geekbench 7 ist proprietär. GUI-Importwerte sind manuell eingegeben und wurden nicht automatisch verifiziert.",
              "Die hier gemessenen OpenGL/Vulkan/CPU-Werte betreffen den **nativen Host**, nicht den QRX68100/QRX86-Gast.",
              "", "## Offen gelassene Pfade", "", "- QRX86-D3D9-HLE: nicht implementiert. Die MIT-D3D9-Referenz liegt separat unter `benchmarks/direct3d9`. ",
              "- Binärkompatible 3dfx Glide-, Warp3D- und AROS-Treiber-Tests: fehlen noch. Die vorhandenen QRX-Tests messen eigene Adapter.", ""]
    (out / "EXTERNAL-REPORT.md").write_text("\n".join(lines), encoding="utf-8")
    # Read-only HTML dashboard with disjoint metric categories. Never chart fake scores.
    sections = []
    for name in ("glmark2", "vkmark", "geekbench7"):
        entry = data["results"].get(name, {})
        details = entry.get("metrics", {"score": entry["score"]} if "score" in entry else {})
        metrics = "".join(f"<dt>{html.escape(str(key))}</dt><dd>{html.escape(str(value))}</dd>" for key, value in details.items())
        sections.append(f"<article><h2>{html.escape(name)}</h2><p>Status: <strong>{html.escape(entry.get('status', 'NOT RUN'))}</strong></p><dl>{metrics}</dl><small>{html.escape(entry.get('reason', ''))}</small></article>")
    # An internal frame counter is not a third-party score; render each category
    # separately without visual normalization, invented values or speedup charts.
    qr = read_json(out / "results.json", {})
    inner = qr.get("results", [])
    internal_rows = "".join("<tr><td>" + html.escape(str(r.get("api", "?"))) +
        "</td><td>" + html.escape(str(r.get("kind", "native GPU / CPU / encoder"))) +
        "</td><td>" + (f"{r['fps']:.1f} FPS" if r.get("status") == "PASS" and isinstance(r.get("fps"), (int, float)) else "—") +
        "</td><td>" + html.escape(str(r.get("status", "?"))) + "</td></tr>" for r in inner)
    internal_html = "<h2>ARMITHLON-eigene Benchmark-Szene</h2>"
    internal_html += ("<p>Noch nicht auf diesem Computer ausgeführt: <code>./armithlon benchmark</code></p>" if not inner else
        "<table><thead><tr><th>API</th><th>Messart</th><th>FPS</th><th>Status</th></tr></thead><tbody>"+internal_rows+"</tbody></table>")
    doc = ("<!doctype html><html lang='de'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>ARMITHLON – Benchmark-Auswertung</title><style>body{font:16px system-ui,sans-serif;background:#10151c;color:#e8eef6;max-width:980px;margin:auto;padding:24px}"
           "main{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:16px}article{border:1px solid #354257;border-radius:12px;padding:18px}"
           "h1{font-size:1.7em}dd{margin:0 0 12px;font-size:1.6em;font-weight:700}dt,small{color:#a4b4c8}"
           "table{width:100%;border-collapse:collapse}td,th{border-bottom:1px solid #354257;text-align:left;padding:8px 6px;font-size:.92em}"
           "footer{color:#a4b4c8;margin-top:25px}</style><h1>ARMITHLON – Benchmark-Status</h1>"
           "<p>Auf diesem Host erhobene Daten. Keine unterschiedlichen Scores gleichsetzen.</p><h2>Externe native Tools</h2><main>"
           + "".join(sections) + "</main>" + internal_html +
           "<footer>Tool-Scores und QRX-FPS sind nicht miteinander vergleichbar. Geekbench-Werte: manueller Import. CPU-FPS, Encoder-FPS und GPU-FPS bleiben getrennt.</footer></html>")
    (out / "benchmarks.html").write_text(doc, encoding="utf-8")


def geekbench_gui() -> int:
    if platform.system() != "Darwin":
        print("Geekbench 7 GUI launcher is macOS-only; install the native version for your platform.")
        print(GEEK_URL)
        return 2
    apps = [Path("/Applications/Geekbench 7.app"), Path.home() / "Applications/Geekbench 7.app"]
    app = next((p for p in apps if p.is_dir()), None)
    if not app:
        print("Install Geekbench 7 from the official site:", GEEK_URL)
        return 2
    return subprocess.call(["open", str(app)])


def import_geekbench(args) -> int:
    metrics = {k: v for k, v in {
        "cpu_single": args.cpu_single, "cpu_multi": args.cpu_multi,
        "gpu_metal": args.gpu_metal, "gpu_vulkan": args.gpu_vulkan}.items() if v is not None}
    if not metrics:
        print("Enter at least one genuine Geekbench 7 score; no values invented", file=sys.stderr)
        return 2
    if any(v <= 0 or v > 1_000_000_000 for v in metrics.values()):
        print("Invalid score; expected positive integer", file=sys.stderr)
        return 2
    if not args.source.strip():
        print("Source note or Geekbench Browser result URL required", file=sys.stderr)
        return 2
    entry = new_entry("geekbench7", "MANUAL", "Values entered from official Geekbench 7 GUI/Browser; not auto-verified",
                      metrics=metrics, version="7", score_unit="Geekbench-7-points",
                      reference=args.source.strip(), verified=False)
    store(args.out.resolve(), entry)
    print("Imported Geekbench 7 metrics:", metrics)
    print("Report:", args.out.resolve() / "EXTERNAL-REPORT.md")
    return 0


def cli(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    for name in ["fetch", "build", "run"]:
        sp = sub.add_parser(name)
        sp.add_argument("tool", nargs="?", choices=[*SOURCES, "all"], default="all")
        if name == "run":
            sp.add_argument("--out", type=Path, default=DEFAULT_REPORT)
            sp.add_argument("--timeout", type=int, default=480)
    sub.add_parser("geekbench")
    imp = sub.add_parser("import-geekbench", help="manually enter genuine Geekbench 7 GUI results")
    imp.add_argument("--cpu-single", type=int)
    imp.add_argument("--cpu-multi", type=int)
    imp.add_argument("--gpu-metal", type=int)
    imp.add_argument("--gpu-vulkan", type=int)
    imp.add_argument("--source", type=str, required=True, help="your result URL or local run note")
    imp.add_argument("--out", type=Path, default=DEFAULT_REPORT)
    inp = parser.parse_args(argv)
    if inp.command == "status": return status()
    if inp.command == "geekbench": return geekbench_gui()
    if inp.command == "import-geekbench": return import_geekbench(inp)
    names = list(SOURCES) if inp.tool == "all" else [inp.tool]
    if inp.command == "fetch": return fetch(names)
    if inp.command == "build": return build(names)
    if inp.command == "run":
        if inp.timeout < 10 or inp.timeout > 7200:
            parser.error("--timeout must be between 10 and 7200 seconds")
        out = inp.out.resolve()
        failed = False
        for name in names:
            entry = run_tool(name, out, inp.timeout)
            store(out, entry)
            print(f"{name:10} {entry['status']:12} {entry.get('score', '')} {entry.get('reason','')}")
            failed = failed or entry["status"] == "ERROR"
        print("Report:", out / "EXTERNAL-REPORT.md")
        return 1 if failed else 0
    return 2

if __name__ == "__main__":
    sys.exit(cli())
