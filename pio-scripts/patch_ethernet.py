Import("env")
import os

LIBDIR = os.path.join(env["PROJECT_LIBDEPS_DIR"], env["PIOENV"], "Ethernet", "src")


def patch_once(path, old, new, label):
    """Replace `old` → `new` exactly once. Skips if `new` already present
    (idempotent). Anchor `old` with surrounding context so commented variants
    of the same value don't false-positive."""
    if not os.path.exists(path):
        print(f"[patch_ethernet] {label}: file missing ({path})")
        return
    with open(path) as f:
        content = f.read()
    if new in content:
        return                          # already patched
    if old not in content:
        print(f"[patch_ethernet] {label}: source marker missing — patch skipped")
        return
    with open(path, "w") as f:
        f.write(content.replace(old, new, 1))
    print(f"[patch_ethernet] Patched {label}")


# 1. EthernetServer.h begin(uint16_t) override (kept from prior version).
es_h = os.path.join(LIBDIR, "EthernetServer.h")
patch_once(
    es_h,
    "virtual void begin();",
    "virtual void begin();\n"
    "    virtual void begin(uint16_t port) override { begin(); }",
    "EthernetServer.h: begin(uint16_t) override",
)

# 2. w5100.h SPI clock — left at lib default 14 MHz. We tried 30 MHz for
#    throughput but some W5500 breakouts (long jumpers, breadboard, weak
#    decoupling) returned LinkUnknown intermittently, tearing down _ethUp.
#    Stable < fast: keep 14 MHz unless you've validated your hardware higher.

# 3. Ethernet.h — enable ETHERNET_LARGE_BUFFERS. With MAX_SOCK_NUM<=2 the
#    W5500 allocates 8 KB RX/TX per socket (vs 2 KB) → ~15 ArtDMX packets of
#    burst headroom before drop. (MAX_SOCK_NUM=2 already set in this file by
#    a prior manual patch; left alone here.)
patch_once(
    os.path.join(LIBDIR, "Ethernet.h"),
    "//#define ETHERNET_LARGE_BUFFERS",
    "#define ETHERNET_LARGE_BUFFERS  // Lumox: 8KB/socket Art-Net burst headroom",
    "Ethernet.h: ETHERNET_LARGE_BUFFERS enabled",
)
