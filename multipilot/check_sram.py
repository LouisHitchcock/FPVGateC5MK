# Fails the build if static data (IRAM, data or BSS) reaches the SRAM bank
# the I/Q dump engine writes into. See src/dump.cpp.
Import("env")  # noqa: F821
import subprocess

LIMIT = 0x40820000
SYMBOLS = ("_iram_end", "_data_end", "_bss_end")


def check(source, target, env):
    elf = str(target[0])
    nm = env.subst("$CC").replace("gcc", "nm")
    out = subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in SYMBOLS:
            found[parts[2]] = int(parts[0], 16)
    bad = False
    for s in SYMBOLS:
        if s not in found:
            print(f"check_sram: {s} not found")
            bad = True
            continue
        v = found[s]
        ok = v <= LIMIT
        bad |= not ok
        print(f"check_sram: {s} = 0x{v:08X} ({'ok' if ok else 'OVER'} limit 0x{LIMIT:08X}, {LIMIT - v:+d} bytes)")
    for sym in ("adctrig", "force_rx_gain"):
        if not any(line.endswith(" T " + sym) for line in out.splitlines()):
            print(f"check_sram: {sym} missing from the image")
            bad = True
    # The cmd_parse stub only stays in if librftest's wifi.o is linked;
    # either way the link resolved.
    has_stub = any(line.endswith(" T cmd_parse") for line in out.splitlines())
    print(f"check_sram: cmd_parse stub {'linked' if has_stub else 'not needed (unreferenced)'}")
    if bad:
        env.Exit(1)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", check)  # noqa: F821
