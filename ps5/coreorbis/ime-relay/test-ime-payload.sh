#!/bin/bash
# Host test of the relay PAYLOAD (ime_relay.c) with stand-in dialog functions (ime_stub.c): builds it for the PC, writes a request over
# its marker the way the app does, runs it and checks the files it writes and what it handed to sceImeDialogInit.
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
${CC:-clang} -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -o "$work/relay" "$here/ime_relay.c" "$here/ime_stub.c"
python3 -I - "$work" <<'PY'
import os, subprocess, sys
work = sys.argv[1]
exe = os.path.join(work, "relay")
blob = open(exe, "rb").read()
marker = b"PS5SX2-IME-RELAY-REQUEST-V1"
# the array g_request: the marker followed by zeros up to 4096 bytes
at = -1
i = 0
while True:
    i = blob.find(marker, i)
    if i < 0:
        break
    if blob[i + len(marker):i + 4096] == b"\0" * (4096 - len(marker)):
        at = i
        break
    i += 1
assert at >= 0, "g_request not found in the payload"

def run(name, text, title, password, mode="", user=77, maxlen=40, typ=0):
    result = os.path.join(work, name + ".txt")
    req = ("PS5SX2-IME-RELAY-REQUEST-V1\nuser=%d\nmax=%d\ntype=%d\npassword=%d\nresult=%s\ntitle=%s\ntext=%s\n"
           % (user, maxlen, typ, password, result, title, text)).encode("utf-8")
    patched = bytearray(blob)
    patched[at:at + len(req)] = req
    patched[at + len(req)] = 0
    path = os.path.join(work, name + ".elf")
    open(path, "wb").write(patched)
    os.chmod(path, 0o755)
    env = dict(os.environ, STUB_REPORT=os.path.join(work, name + ".report"))
    if mode:
        env["STUB_MODE"] = mode
    subprocess.run([path], env=env, timeout=30, check=False)
    out = {}
    for suffix in ("", ".started", ".log"):
        p = result + suffix
        out[suffix] = open(p, "rb").read().decode("utf-8", "replace") if os.path.exists(p) else None
    rep = os.path.join(work, name + ".report")
    out["report"] = open(rep).read() if os.path.exists(rep) else None
    return out

fail = 0
def check(cond, what, extra=""):
    global fail
    if not cond:
        fail += 1
        print("FAIL:", what, extra)

# 1. text in, text out: the buffer the dialog saw holds the start text (UTF-16), the result file holds start text + typed.
o = run("t1", "Grüße \U0001F3AE", "Search games", 0)
check(o[""] is not None and o[""].startswith("status 2\nend 0\nrc 0\ntext "), "result written", repr(o[""]))
check(o[""] and o[""].split("text ", 1)[1].rstrip("\n") == "Grüße \U0001F3AEü\U0001F600", "text round trip", repr(o[""]))
check(o[".started"] is not None, "started file")
check(o["report"] and "user 77" in o["report"] and "type 0" in o["report"] and "max 40" in o["report"], "dialog parameters", repr(o["report"]))
check(o["report"] and "option 0x22" in o["report"], "options: no auto-capital + no learning", repr(o["report"]))
check(o["report"] and "title16 0053 0065 0061 0072 0063 0068 0020 0067 0061 006d 0065 0073" in o["report"], "title in UTF-16", repr(o["report"]))
check(o["report"] and "text16 0047 0072 00fc 00df 0065 0020 d83c dfae" in o["report"], "start text in UTF-16 incl. surrogates", repr(o["report"]))
check(o[".log"] and "sceImeDialogInit: 0" in o[".log"], "log has the init", repr(o[".log"]))

# 2. a password: option bit 0x4 set, basic Latin type
o = run("t2", "", "Password", 1, typ=1)
check(o["report"] and "option 0x26" in o["report"] and "type 1" in o["report"], "password options", repr(o["report"]))

# 3. cancelled: end 1, no text handed back
o = run("t3", "keep me", "x", 0, mode="cancel")
check(o[""] and o[""].startswith("status 2\nend 1\n") and o[""].endswith("text \n"), "cancel result", repr(o[""]))

# 4. the dialog can't start: status -1 and the error code, quickly
o = run("t4", "x", "x", 0, mode="fail")
check(o[""] and o[""].startswith("status -1\n") and "rc 80020063" in o[""], "failure result", repr(o[""]))
check(o[".started"] is None, "no started file on failure")

# 5. max length outside the range falls back to 64
o = run("t5", "", "x", 0, maxlen=9999)
check(o["report"] and "max 64" in o["report"], "max length clamp", repr(o["report"]))

# 6. a payload sent without a request does nothing (no files)
blob0 = bytes(blob)
path = os.path.join(work, "empty.elf")
open(path, "wb").write(blob0)
os.chmod(path, 0o755)
r = subprocess.run([path], timeout=30)
check(r.returncode == 1, "no request: exits 1", str(r.returncode))

print("ime payload test: %s" % ("all passed" if not fail else "%d FAILED" % fail))
sys.exit(1 if fail else 0)
PY
