"""Run every ROM in <drawer>/roms on an Amiga with the GenesisPlusGX
installed there, take two screenshots (title at 10 s, after a Start press
at 24 s), stop with Esc and keep the exit log.

The Amiga is driven through the amimcp client, set up with environment
variables:
  AMIGA_HOST     host name or address of the Amiga (required)
  AMIGA_TOKEN    amimcp token of that Amiga (required)
  AMIMCP_SERVER  directory holding amimcp's amiga.py and png.py (required)
  GPGX_DRAWER    drawer of the emulator on the Amiga
                 (default stuff:emulators/sega)
  GPGX_ROMLIST   text file with the ROMs to run, one name per line relative
                 to <drawer>/roms (default: roms.txt next to this script)

Output in this directory: ag_<nn>_a.png, ag_<nn>_b.png, ag_<nn>.log and
ag_summary.txt (one line per game: emulation us/frame, drawn %, audio
dropped/dry, resyncs, cheats line, and a flag when the log has no summary).
"""
import os
import sys
import time


def env(name):
    value = os.environ.get(name)
    if not value:
        sys.exit("allgames.py: environment variable %s is not set "
                 "(see the top of this file)" % name)
    return value


HOST = env("AMIGA_HOST")
TOKEN = env("AMIGA_TOKEN")
sys.path.insert(0, env("AMIMCP_SERVER"))
from amiga import Amiga  # noqa: E402
import png  # noqa: E402

DRAWER = os.environ.get("GPGX_DRAWER") or "stuff:emulators/sega"
LOG = DRAWER + "/ag.log"

ROMLIST = os.environ.get("GPGX_ROMLIST") or os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "roms.txt")


def read_roms():
    """ROM names relative to <drawer>/roms, one per line; '#' starts a
    comment line."""
    try:
        text = open(ROMLIST, "rb").read().decode("latin-1")
    except OSError as e:
        sys.exit("allgames.py: cannot read the ROM list %s: %s" % (ROMLIST, e))
    roms = [l.strip() for l in text.splitlines()]
    roms = [l for l in roms if l and not l.startswith("#")]
    if not roms:
        sys.exit("allgames.py: the ROM list %s is empty" % ROMLIST)
    return roms


def shot(a, name):
    try:
        s = a.screenshot()
        open(name, "wb").write(png.screenshot_png(s))
        return "%dx%d" % (s.width, s.height)
    except Exception as e:  # noqa: BLE001
        return "shot failed: %s" % e


def pick(text, prefix):
    for line in text.splitlines():
        if line.startswith(prefix):
            return line[len(prefix):].strip()
    return None


def main():
    a = Amiga(HOST, token=TOKEN, timeout=60.0)
    start = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    out = open("ag_summary.txt", "a")
    for n, rom in enumerate(read_roms()):
        if n < start:
            continue
        tag = "ag_%02d" % n
        print("==", n, rom, flush=True)
        cmd = ('Run >NIL: %s/GenesisPlusGX >%s "%s/roms/%s" -nogl -fps'
               % (DRAWER, LOG, DRAWER, rom))
        rc, o = a.exec_command(cmd, timeout=20)
        time.sleep(10)
        sa = shot(a, tag + "_a.png")
        a.input_key("return")
        time.sleep(0.3)
        a.input_key("return")
        time.sleep(14)
        sb = shot(a, tag + "_b.png")
        a.input_key("esc")
        time.sleep(4)
        try:
            log = a.read_file(LOG)
            if isinstance(log, bytes):
                log = log.decode("latin-1")
        except Exception as e:  # noqa: BLE001
            log = "LOG READ FAILED: %s" % e
        open(tag + ".log", "w", encoding="latin-1").write(log)
        fr = pick(log, "Frames: budget")
        drawn = pick(log, "Frames: ") if False else None
        drawn = [l for l in log.splitlines() if l.startswith("Frames:") and "drawn" in l]
        aud = [l for l in log.splitlines() if l.startswith("Audio:") and "dropped" in l]
        rs = [l for l in log.splitlines() if "resyncs" in l]
        ch = [l for l in log.splitlines() if l.startswith("Cheat")]
        sysl = pick(log, "System:")
        ok = "OK" if fr else "NO SUMMARY"
        line = ("%02d %-60s | %s | %s | %s | %s | %s | %s | %s | shots %s %s"
                % (n, rom[:60], ok, sysl, fr, drawn[0][8:] if drawn else "-",
                   aud[0][7:] if aud else "-", rs[0].split("drift")[1] if rs else "-",
                   "; ".join(ch) if ch else "no cheats", sa, sb))
        print(line, flush=True)
        out.write(line + "\n")
        out.flush()
        if not fr:
            # emulator may still be running or dead: check the front screen
            try:
                scr = a.screens()
                print("   screens:", scr, flush=True)
            except Exception as e:  # noqa: BLE001
                print("   screens failed:", e, flush=True)
            # try to get back to a known state before the next game
            a.input_key("esc")
            time.sleep(3)
    out.close()
    print("done", flush=True)


if __name__ == "__main__":
    main()
