Import("env")
import os

# esp_dmx 4.1 was written against IDF <= 5.2. IDF 5.3 (shipped with
# arduino-esp32 v3.1.x) removed `uart_signal_conn_t.module` in favour of
# direct PERIPH_UART{N}_MODULE enums. We patch the three call sites in
# uart.c to compute the module enum from dmx_num instead.
#
# PERIPH_UART0_MODULE..2_MODULE are sequential in soc/periph_defs.h, so
# (PERIPH_UART0_MODULE + dmx_num) is safe across ESP32/S2/S3/C-series.

LIB_FILE = os.path.join(env["PROJECT_LIBDEPS_DIR"], env["PIOENV"],
                        "esp_dmx", "src", "dmx", "hal", "uart.c")


def patch_once(path, old, new, label):
    if not os.path.exists(path):
        print(f"[patch_esp_dmx] {label}: file missing ({path})")
        return
    with open(path) as f:
        content = f.read()
    if new in content:
        return                          # already patched
    if old not in content:
        print(f"[patch_esp_dmx] {label}: marker missing — patch skipped")
        return
    with open(path, "w") as f:
        f.write(content.replace(old, new))
    print(f"[patch_esp_dmx] Patched {label}")


# All three call sites use the same expression — single replace_all is fine
# but we anchor with the surrounding line for safety.
patch_once(
    LIB_FILE,
    "periph_module_enable(uart_periph_signal[dmx_num].module);",
    "periph_module_enable((periph_module_t)(PERIPH_UART0_MODULE + dmx_num));",
    "uart.c: periph_module_enable",
)
patch_once(
    LIB_FILE,
    "periph_module_reset(uart_periph_signal[dmx_num].module);",
    "periph_module_reset((periph_module_t)(PERIPH_UART0_MODULE + dmx_num));",
    "uart.c: periph_module_reset",
)
patch_once(
    LIB_FILE,
    "periph_module_disable(uart_periph_signal[uart->num].module);",
    "periph_module_disable((periph_module_t)(PERIPH_UART0_MODULE + uart->num));",
    "uart.c: periph_module_disable",
)
