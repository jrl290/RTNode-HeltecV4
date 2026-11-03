#!/usr/bin/env python3
"""Generate the board check's board library from Meshtastic's board definitions.

Meshtastic (https://github.com/meshtastic/firmware, GPL-3.0) describes each
board in variants/<chip>/<board>/: a platformio.ini with its build flags and a
variant.h with its pins. Some pins are named after the Arduino pins file the
board builds with (SDA_OLED, Vext), so each variant.h is run through the C
preprocessor together with that file, and every name is resolved to a number.

Usage:
  gen_board_library.py --meshtastic <firmware checkout> --chip esp32s3 \
      --framework ~/.platformio/packages/framework-arduinoespressif32 \
      --pio-boards ~/.platformio/platforms/espressif32/boards \
      --header ../src/board_library.h --json ../../docs/board-check/boards.json

Anything that can't be resolved is logged, not guessed.
"""

import argparse
import ast
import json
import os
import re
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------- ini files

def parse_ini(path):
    """PlatformIO ini: sections, keys, indented continuation lines."""
    sections = {}
    cur = None
    key = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\n")
            stripped = line.strip()
            if not stripped or stripped.startswith(";") or stripped.startswith("#"):
                continue
            m = re.match(r"^\[(.+)\]\s*$", stripped)
            if m and not line[0].isspace():
                cur = sections.setdefault(m.group(1).strip(), {})
                key = None
                continue
            if cur is None:
                continue
            if line[0].isspace() and key is not None:
                cur[key] += "\n" + stripped
                continue
            m = re.match(r"^([A-Za-z0-9_.\-]+)\s*=\s*(.*)$", stripped)
            if m:
                key = m.group(1)
                cur[key] = m.group(2)
    return sections


def strip_comment(v):
    return re.sub(r"\s;.*$", "", v).strip()


class Ini:
    def __init__(self, files):
        self.sections = {}
        for p in files:
            for name, keys in parse_ini(p).items():
                self.sections.setdefault(name, {}).update(keys)

    def get(self, section, key, depth=0):
        if depth > 12 or section not in self.sections:
            return None
        s = self.sections[section]
        if key in s:
            return self.interp(s[key], depth)
        ext = s.get("extends")
        if ext:
            for parent in [e.strip() for e in ext.split(",")]:
                v = self.get(parent, key, depth + 1)
                if v is not None:
                    return v
        return None

    def interp(self, value, depth):
        def rep(m):
            sec, k = m.group(1), m.group(2)
            v = self.get(sec, k, depth + 1)
            return v if v is not None else ""
        return re.sub(r"\$\{([^.}]+)\.([^}]+)\}", rep, value)


def build_flag_tokens(flags):
    out = []
    for line in flags.split("\n"):
        line = strip_comment(line)
        if line:
            out.extend(line.split())
    return out


def parse_flags(tokens):
    defines, includes = {}, []
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t in ("-D", "-I") and i + 1 < len(tokens):
            arg = tokens[i + 1]
            i += 2
            if t == "-D":
                put_define(defines, arg)
            else:
                includes.append(arg)
            continue
        if t.startswith("-D"):
            put_define(defines, t[2:])
        elif t.startswith("-I"):
            includes.append(t[2:])
        i += 1
    return defines, includes


def put_define(defines, arg):
    if "=" in arg:
        k, v = arg.split("=", 1)
        defines[k] = v
    else:
        defines[arg] = ""

# ------------------------------------------------------------ preprocessing

def find_includes(path):
    names = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', line)
            if m:
                names.append(m.group(1))
    return names


def preprocess(variant_h, pins_h, inc_dirs, defines):
    """Return {macro: body} after the preprocessor has run over both files."""
    with tempfile.TemporaryDirectory() as stub:
        # Stub every include that isn't in the variant's own folders, and the
        # system headers, so only the board's own definitions are read.
        todo = [variant_h] + ([pins_h] if pins_h else [])
        seen = set()
        while todo:
            p = todo.pop()
            if p in seen:
                continue
            seen.add(p)
            for name in find_includes(p):
                found = None
                for d in inc_dirs:
                    c = os.path.join(d, name)
                    if os.path.isfile(c):
                        found = c
                        break
                if found:
                    todo.append(found)
                else:
                    sp = os.path.join(stub, name)
                    os.makedirs(os.path.dirname(sp), exist_ok=True)
                    open(sp, "a").close()
        cmd = ["clang", "-E", "-dM", "-x", "c++", "-nostdinc", "-undef", "-I", stub]
        for d in inc_dirs:
            cmd += ["-I", d]
        for k, v in defines.items():
            if '"' in v or "\\" in v:
                continue
            cmd.append("-D%s=%s" % (k, v) if v != "" else "-D%s" % k)
        if pins_h:
            cmd += ["-include", pins_h]
        cmd += ["-include", variant_h, "/dev/null"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.strip().splitlines()[-1] if r.stderr else "cpp failed")
    macros = {}
    for line in r.stdout.splitlines():
        m = re.match(r"^#define\s+([A-Za-z_][A-Za-z0-9_]*)(\([^)]*\))?\s*(.*)$", line)
        if m and not m.group(2):
            macros[m.group(1)] = m.group(3).strip()
    return macros


def pins_consts(pins_h):
    """static const uint8_t NAME = VALUE; lines of an Arduino pins file."""
    out = {}
    if not pins_h:
        return out
    with open(pins_h, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"^\s*static\s+const\s+\w+\s+([A-Za-z_]\w*)\s*=\s*([^;]+);", line)
            if m:
                out[m.group(1)] = m.group(2).strip()
    return out

# --------------------------------------------------------------- evaluation

BUILTIN = {"HIGH": "1", "LOW": "0", "true": "1", "false": "0", "NULL": "0"}


class Board:
    def __init__(self, macros, consts):
        self.m = macros
        self.c = consts

    def has(self, name):
        return name in self.m

    def raw(self, name):
        return self.m.get(name)

    def expand(self, text, depth=0):
        if depth > 20:
            return None
        def rep(mo):
            w = mo.group(0)
            if w in BUILTIN:
                return BUILTIN[w]
            g = re.match(r"^GPIO_NUM_(\d+)$", w)
            if g:
                return g.group(1)
            if w in self.m:
                v = self.expand(self.m[w], depth + 1)
                return "(" + v + ")" if v not in (None, "") else "__UNRESOLVED__"
            if w in self.c:
                v = self.expand(self.c[w], depth + 1)
                return "(" + v + ")" if v not in (None, "") else "__UNRESOLVED__"
            return "__UNRESOLVED__"
        t = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+\.\d*|\d+)[uUlLfF]*\b", lambda mo: mo.group(1), text)
        t = re.sub(r"\b[A-Za-z_]\w*\b", rep, t)
        t = re.sub(r"\(\s*u?int\d+_t\s*\)", "", t)
        return t

    def num(self, name, default=None):
        """Numeric value of a macro, or default when absent/unresolvable."""
        if name not in self.m:
            return default
        body = self.m[name]
        if body == "":
            return default
        t = self.expand(body)
        if t is None or "__UNRESOLVED__" in t:
            return default
        try:
            return safe_eval(t)
        except Exception:
            return default

    def first_num(self, names, default=None):
        for n in names:
            v = self.num(n)
            if v is not None:
                return v
        return default


def safe_eval(expr):
    node = ast.parse(expr.strip(), mode="eval").body
    def ev(n):
        if isinstance(n, ast.Constant) and isinstance(n.value, (int, float)):
            return n.value
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, (ast.USub, ast.UAdd)):
            v = ev(n.operand)
            return -v if isinstance(n.op, ast.USub) else v
        if isinstance(n, ast.BinOp):
            a, b = ev(n.left), ev(n.right)
            ops = {ast.Add: lambda: a + b, ast.Sub: lambda: a - b, ast.Mult: lambda: a * b,
                   ast.Div: lambda: a / b, ast.LShift: lambda: a << b, ast.BitOr: lambda: a | b}
            for k, f in ops.items():
                if isinstance(n.op, k):
                    return f()
        raise ValueError("unsupported")
    return ev(node)

# ------------------------------------------------------------ board entries

def pin(v):
    if v is None:
        return -1
    v = int(v)
    return v if 0 <= v <= 48 else -1


RADIO_CHIPS = [
    ("sx126x", ["USE_SX1262", "USE_SX1268", "USE_LLCC68", "USE_SX1261"]),
    ("sx127x", ["USE_RF95"]),
    ("sx128x", ["USE_SX1280"]),
    ("lr11xx", ["USE_LR1110", "USE_LR1120", "USE_LR1121"]),
    ("lr2021", ["USE_LR2021"]),
]


def radios(b):
    out = []
    for kind, flags in RADIO_CHIPS:
        present = [f[4:] for f in flags if b.has(f)]
        if not present:
            continue
        r = {"type": kind, "chips": present}
        if kind == "sx126x":
            r.update(cs=b.first_num(["SX126X_CS", "LORA_CS"]), rst=b.first_num(["SX126X_RESET", "LORA_RESET"]),
                     irq=b.first_num(["SX126X_DIO1", "LORA_DIO1"]), busy=b.first_num(["SX126X_BUSY", "LORA_DIO2"]),
                     tcxo=b.num("SX126X_DIO3_TCXO_VOLTAGE"), rf_switch=b.has("SX126X_DIO2_AS_RF_SWITCH"),
                     txen=b.num("SX126X_TXEN"), rxen=b.num("SX126X_RXEN"))
        elif kind == "sx127x":
            r.update(cs=b.first_num(["RF95_NSS", "LORA_CS"]), rst=b.first_num(["RF95_RESET", "LORA_RESET"]),
                     irq=b.first_num(["RF95_IRQ", "LORA_DIO0"]), busy=None, tcxo=None, rf_switch=False,
                     txen=b.num("RF95_TXEN"), rxen=b.num("RF95_RXEN"))
        elif kind == "sx128x":
            r.update(cs=b.first_num(["SX128X_CS", "LORA_CS"]), rst=b.first_num(["SX128X_RESET", "LORA_RESET"]),
                     irq=b.first_num(["SX128X_DIO1", "LORA_DIO1"]), busy=b.first_num(["SX128X_BUSY", "LORA_DIO2"]),
                     tcxo=None, rf_switch=False, txen=b.num("SX128X_TXEN"), rxen=b.num("SX128X_RXEN"))
        elif kind == "lr11xx":
            p = [c for c in ("LR1110", "LR1120", "LR1121") if b.has(c + "_SPI_NSS_PIN")]
            p = p[0] if p else "LR1110"
            r.update(cs=b.first_num([p + "_SPI_NSS_PIN", "LORA_CS"]), rst=b.first_num([p + "_NRESET_PIN", "LORA_RESET"]),
                     irq=b.first_num([p + "_IRQ_PIN", "LORA_DIO1"]), busy=b.first_num([p + "_BUSY_PIN", "LORA_DIO2"]),
                     tcxo=b.num("LR11X0_DIO3_TCXO_VOLTAGE"), rf_switch=b.has("LR11X0_DIO_AS_RF_SWITCH"),
                     txen=None, rxen=None)
            r["sck"] = b.num(p + "_SPI_SCK_PIN")
            r["miso"] = b.num(p + "_SPI_MISO_PIN")
            r["mosi"] = b.num(p + "_SPI_MOSI_PIN")
        else:
            r.update(cs=b.num("LORA_CS"), rst=b.num("LORA_RESET"), irq=b.num("LORA_DIO1"), busy=b.num("LORA_DIO2"),
                     tcxo=None, rf_switch=False, txen=None, rxen=None)
        out.append(r)
    return out


def screen(b):
    if b.has("HAS_SCREEN") and b.num("HAS_SCREEN") == 0:
        return {"type": "none"}
    if b.has("USE_EINK"):
        return {"type": "eink"}
    for flag, t in (("USE_SSD1306", "ssd1306"), ("USE_SH1106", "sh1106"),
                    ("USE_SH1107_128_64", "sh1107"), ("USE_SH1107", "sh1107")):
        if b.has(flag):
            return {"type": t, "rst": b.num("RESET_OLED")}
    for prefix, t in (("ST7789", "st7789"), ("ST7735", "st7735"), ("ST7796", "st7796"), ("ILI9341", "ili9341")):
        if b.has(prefix + "_CS"):
            return {"type": t, "sck": b.num(prefix + "_SCK"), "mosi": b.num(prefix + "_SDA"),
                    "miso": b.num(prefix + "_MISO"), "cs": b.num(prefix + "_CS"), "dc": b.num(prefix + "_RS"),
                    "rst": b.num(prefix + "_RESET"), "bl": b.first_num([prefix + "_BL", "TFT_BL"]),
                    "bl_on": b.num("TFT_BACKLIGHT_ON", 1),
                    "power": b.num("VTFT_CTRL"), "power_on": b.num("VTFT_CTRL_ON", 0),
                    "w": b.num("TFT_WIDTH"), "h": b.num("TFT_HEIGHT")}
    if b.has("USE_TFTDISPLAY") or b.has("HAS_TFT") or b.has("HAS_SPI_TFT") or b.has("USE_ARDUINO_GFX"):
        return {"type": "tft_other"}
    return {"type": "oled_maybe", "rst": b.num("RESET_OLED")}


BUTTON_NAMES = ["BUTTON_PIN", "BUTTON_PIN_ALT", "ALT_BUTTON_PIN", "PIN_BUTTON1", "PIN_BUTTON2",
                "CANCEL_BUTTON_PIN", "BUTTON_PIN_TOUCH", "TB_PRESS", "TB_UP", "TB_DOWN", "TB_LEFT", "TB_RIGHT"]

PIN_LIKE = re.compile(r"(PIN|_TX|_RX|_EN\b|_EN_|_CS\b|SCK|MOSI|MISO|SDA|SCL|IRQ|_INT\b|RST|RESET|DIO\d|BUSY|_BL\b|"
                      r"POWER|PWR|CTRL|LED|BUZZER|CLK|BCK|DOUT|DIN|LRCK|MCLK|_WS\b|PPS|STANDBY|WAKEUP|_RS\b|_DC\b|"
                      r"PA_|CSD|CTX|SDCARD|_SS\b)")
NOT_PIN = re.compile(r"(THRESHOLD|INTERVAL|DEFAULT|FREQ|_MS\b|SIZE|ADDR|MODEL|VALUE|_ON\b|LEVEL|ENABLED|CHANNEL|"
                     r"ROTATION|OFFSET|WIDTH|HEIGHT|VOLTAGE|MULTIPLIER|ATTEN|ACTIVE|MODE|PORT|HOST|COUNT|NUM_|BAUD|"
                     r"_ON_|STATE|DIRECTION|WARMUP|SPEED|TIMEOUT|GAIN|BRIGHT|INVERT|ORDER|OUTPUT|DIV|RATIO|MAX|MIN)")


def other_pins(b, variant_macro_names, exclude):
    out = set()
    for name in variant_macro_names:
        if not PIN_LIKE.search(name) or NOT_PIN.search(name) or name in BUTTON_NAMES:
            continue
        v = b.num(name)
        if isinstance(v, (int, float)) and float(v).is_integer() and 0 <= int(v) <= 48:
            out.add(int(v))
    return sorted(out - set(exclude))


# Pins that switch on power to the radio, screen or other parts, with their
# "on" level, in the order Meshtastic's start-up sets them.
POWER_SWITCHES = [("VEXT_ENABLE", "VEXT_ON_VALUE", 0), ("PIN_POWER_EN", None, 1), ("KB_POWERON", None, 1),
                  ("SX126X_POWER_EN", None, 1), ("RF95_POWER_EN", None, 1), ("LR11X0_POWER_EN", None, 1),
                  ("LR2021_POWER_EN", None, 1), ("LORA_PA_POWER", None, 1), ("TFT_EN", "TFT_EN_ON", 1)]


def power_pins(b):
    out = []
    for name, level, default in POWER_SWITCHES:
        p = pin(b.num(name))
        if p >= 0 and p not in [o["pin"] for o in out]:
            out.append({"pin": p, "on": int(b.num(level, default)) if level else default})
    return out


def board_meta(env, ini, pio_boards, mesh_boards):
    bname = strip_comment(ini.get("env:" + env, "board") or "") or None
    j = None
    for d in (mesh_boards, pio_boards):
        p = os.path.join(d, (bname or "") + ".json")
        if bname and os.path.isfile(p):
            with open(p) as f:
                j = json.load(f)
            break
    meta = {"board": bname, "mcu": None, "flash_mb": None, "psram": "unknown", "usb": "any", "arduino_variant": None}
    if not j:
        return meta
    build, upload = j.get("build", {}), j.get("upload", {})
    meta["mcu"] = build.get("mcu")
    meta["arduino_variant"] = build.get("variant")
    fs = strip_comment(ini.get("env:" + env, "board_upload.flash_size") or upload.get("flash_size") or "")
    m = re.match(r"^(\d+)\s*MB$", fs, re.I)
    if m:
        meta["flash_mb"] = int(m.group(1))
    flags = " ".join(build.get("extra_flags", [])) if isinstance(build.get("extra_flags"), list) else str(build.get("extra_flags", ""))
    flags += " " + (ini.get("env:" + env, "build_flags") or "")
    mem = strip_comment(ini.get("env:" + env, "board_build.arduino.memory_type") or build.get("arduino", {}).get("memory_type", ""))
    if "BOARD_HAS_PSRAM" in flags:
        meta["psram"] = "octal" if mem.endswith("opi") else "quad" if mem.endswith("qspi") else "yes"
    else:
        meta["psram"] = "none"
    hw = build.get("hwids") or []
    if hw and isinstance(hw[0], list):
        vid = hw[0][0].lower()
        meta["usb"] = "native" if vid == "0x303a" else "bridge"
    return meta


def variant_macro_names(variant_h, defines):
    names = set(defines)
    with open(variant_h, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"^\s*#\s*define\s+([A-Za-z_]\w*)", line)
            if m:
                names.add(m.group(1))
    return names


def generate(args):
    root = args.meshtastic
    vroot = os.path.join(root, "variants", args.chip)
    inis = []
    for dp, dn, fn in os.walk(os.path.join(root, "variants")):
        for f in fn:
            if f.endswith(".ini"):
                inis.append(os.path.join(dp, f))
    ini = Ini(sorted(inis))
    entries, log = [], []
    for dp, dn, fn in sorted(os.walk(vroot)):
        if "platformio.ini" not in fn:
            continue
        for section in parse_ini(os.path.join(dp, "platformio.ini")):
            if not section.startswith("env:"):
                continue
            env = section[4:]
            flags = ini.get(section, "build_flags") or ""
            defines, includes = parse_flags(build_flag_tokens(flags))
            inc_dirs = [os.path.join(root, i) for i in includes if not os.path.isabs(i)]
            inc_dirs = [d for d in inc_dirs if os.path.isdir(d)]
            variant_h = next((os.path.join(d, "variant.h") for d in inc_dirs
                              if os.path.isfile(os.path.join(d, "variant.h"))), None)
            if not variant_h:
                log.append("%s: no variant.h on its include path, skipped" % env)
                continue
            meta = board_meta(env, ini, args.pio_boards, os.path.join(root, "boards"))
            if meta["mcu"] and meta["mcu"] != args.chip:
                log.append("%s: board file says %s, skipped" % (env, meta["mcu"]))
                continue
            pins_h = next((os.path.join(d, "pins_arduino.h") for d in inc_dirs
                           if os.path.isfile(os.path.join(d, "pins_arduino.h"))), None)
            if not pins_h and meta["arduino_variant"]:
                p = os.path.join(args.framework, "variants", meta["arduino_variant"], "pins_arduino.h")
                if os.path.isfile(p):
                    pins_h = p
                    inc_dirs.append(os.path.dirname(p))
                else:
                    log.append("%s: Arduino pins file for %s not found" % (env, meta["arduino_variant"]))
            try:
                macros = preprocess(variant_h, pins_h, inc_dirs, defines)
            except RuntimeError as e:
                log.append("%s: preprocessor: %s" % (env, e))
                continue
            b = Board(macros, pins_consts(pins_h))
            rad = radios(b)
            scr = screen(b)
            pmu = "axp2101" if b.has("HAS_AXP2101") else "axp192" if b.has("HAS_AXP192") else None
            sda, scl = b.num("I2C_SDA"), b.num("I2C_SCL")
            if sda is None and scl is None:
                sda, scl = b.num("SDA"), b.num("SCL")
            touch = None
            if b.has("HAS_TOUCHSCREEN") and b.num("HAS_TOUCHSCREEN", 1) != 0:
                touch = {"int": b.first_num(["SCREEN_TOUCH_INT", "TOUCH_INT_PIN"]),
                         "rst": b.first_num(["SCREEN_TOUCH_RST", "TOUCH_RST_PIN"]),
                         "addr": b.num("TOUCH_SLAVE_ADDRESS"), "bus": b.num("TOUCH_I2C_PORT", 0)}
            buttons = []
            for n in BUTTON_NAMES:
                v = pin(b.num(n))
                if v >= 0 and v not in buttons:
                    buttons.append(v)
            e = {
                "id": env,
                "name": (ini.get(section, "custom_meshtastic_display_name") or env).strip(),
                "supported": (ini.get(section, "custom_meshtastic_actively_supported") or "").strip() == "true",
                "flash_mb": meta["flash_mb"], "psram": meta["psram"], "usb": meta["usb"],
                "lora": {"sck": b.num("LORA_SCK"), "miso": b.num("LORA_MISO"), "mosi": b.num("LORA_MOSI")},
                "radios": rad,
                "power": power_pins(b),
                "pmu": pmu, "pmu_bus": 1 if b.has("PMU_USE_WIRE1") else 0,
                "i2c": [sda, scl, b.num("I2C_SDA1"), b.num("I2C_SCL1")],
                "screen": scr, "touch": touch, "buttons": buttons,
                "led": b.first_num(["LED_PIN", "LED_POWER"]), "led_on": b.num("LED_STATE_ON", 1),
                "battery": b.num("BATTERY_PIN"), "adc_mult": b.num("ADC_MULTIPLIER"),
                "adc_ctrl": b.num("ADC_CTRL"), "adc_ctrl_on": b.num("ADC_CTRL_ENABLED", 1),
            }
            used = set()
            for r in rad:
                used |= {pin(r.get(k)) for k in ("cs", "rst", "irq", "busy", "txen", "rxen", "sck", "miso", "mosi")}
            used |= {pin(e["lora"][k]) for k in ("sck", "miso", "mosi")}
            used |= {pin(x) for x in e["i2c"]}
            used |= {pin(scr.get(k)) for k in ("sck", "mosi", "miso", "cs", "dc", "rst", "bl", "power")}
            used.discard(-1)
            e["other_pins"] = other_pins(b, variant_macro_names(variant_h, defines), used | set(buttons))
            if not rad:
                log.append("%s: no radio found in its definition" % env)
            for r in rad:
                missing = [k for k in ("cs", "rst", "irq") if r.get(k) is None]
                if r["type"] in ("sx126x", "sx128x", "lr11xx") and r.get("busy") is None:
                    missing.append("busy")
                if missing:
                    log.append("%s: %s pins unresolved: %s" % (env, r["type"], ", ".join(missing)))
            if any(e["lora"][k] is None for k in ("sck", "miso", "mosi")) and not any("sck" in r for r in rad):
                log.append("%s: LoRa SPI pins unresolved" % env)
            entries.append(e)
    return entries, log

# ------------------------------------------------------------------ output

RADIO_ENUM = {"sx126x": "RADIO_SX126X", "sx127x": "RADIO_SX127X", "sx128x": "RADIO_SX128X",
              "lr11xx": "RADIO_LR11XX", "lr2021": "RADIO_LR2021"}
SCREEN_ENUM = {"none": "SCREEN_NONE", "eink": "SCREEN_EINK", "ssd1306": "SCREEN_SSD1306", "sh1106": "SCREEN_SH1106",
               "sh1107": "SCREEN_SH1107", "st7789": "SCREEN_ST7789", "st7735": "SCREEN_ST7735",
               "st7796": "SCREEN_TFT_OTHER", "ili9341": "SCREEN_TFT_OTHER", "tft_other": "SCREEN_TFT_OTHER",
               "oled_maybe": "SCREEN_OLED_MAYBE"}
PSRAM_ENUM = {"none": "PSRAM_NONE", "quad": "PSRAM_QUAD", "octal": "PSRAM_OCTAL", "yes": "PSRAM_ANY", "unknown": "PSRAM_ANY"}
USB_ENUM = {"native": "USB_NATIVE", "bridge": "USB_BRIDGE", "any": "USB_ANY"}


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def c_pins(vals, n):
    vals = [pin(v) for v in vals][:n]
    vals += [-1] * (n - len(vals))
    return "{" + ", ".join(str(v) for v in vals) + "}"


def write_header(entries, path, source_note):
    lines = ["// Generated by board-check/tools/gen_board_library.py. Do not edit by hand.",
             "// Board definitions from the Meshtastic firmware project (GPL-3.0),",
             "// https://github.com/meshtastic/firmware, " + source_note + ".",
             "#pragma once", '#include "board_types.h"', "",
             "static const BoardDef BOARD_LIBRARY[] = {"]
    for e in entries:
        rs = []
        for r in e["radios"][:3]:
            tcxo = int(round((r.get("tcxo") or 0) * 10))
            rs.append("{%s, %d, %d, %d, %d, %d, %s, %d, %d, %d, %d, %d}" % (
                RADIO_ENUM[r["type"]], pin(r.get("cs")), pin(r.get("rst")), pin(r.get("irq")), pin(r.get("busy")),
                tcxo, "true" if r.get("rf_switch") else "false", pin(r.get("txen")), pin(r.get("rxen")),
                pin(r.get("sck", e["lora"]["sck"])), pin(r.get("miso", e["lora"]["miso"])),
                pin(r.get("mosi", e["lora"]["mosi"]))))
        while len(rs) < 3:
            rs.append("{RADIO_NONE, -1, -1, -1, -1, 0, false, -1, -1, -1, -1, -1}")
        pw = ["{%d, %d}" % (pin(p["pin"]), int(p["on"] or 0)) for p in e["power"][:4]]
        pws = ", ".join(pw + ["{-1, 0}"] * (4 - len(pw)))
        s = e["screen"]
        t = e["touch"] or {}
        lines.append("  { // " + e["name"])
        lines.append("    %s, %s, %s, %d, %s," % (c_str(e["id"]), c_str(e["name"]), PSRAM_ENUM[e["psram"]],
                                                   e["flash_mb"] or 0, USB_ENUM[e["usb"]]))
        lines.append("    {" + ", ".join(rs) + "},")
        lines.append("    {%s}, %s, %d," % (pws, {"axp2101": "PMU_AXP2101", "axp192": "PMU_AXP192"}.get(e["pmu"], "PMU_NONE"),
                                           e["pmu_bus"]))
        lines.append("    %s," % c_pins(e["i2c"], 4))
        lines.append("    %s, %s, %d, %d, %d, %d," % (
            SCREEN_ENUM[s["type"]],
            c_pins([s.get(k) for k in ("sck", "mosi", "miso", "cs", "dc", "rst", "bl", "power")], 8),
            int(s.get("bl_on") if s.get("bl_on") is not None else 1), int(s.get("power_on") or 0),
            int(s.get("w") or 0), int(s.get("h") or 0)))
        lines.append("    %s, %d, %d, %d, 0x%02X," % (
            "true" if e["touch"] else "false", pin(t.get("int")), pin(t.get("rst")), int(t.get("bus") or 0),
            int(t.get("addr") or 0)))
        lines.append("    %s, %d, %d, %d, %d, %d, %d," % (
            c_pins(e["buttons"], 6), pin(e["led"]), int(e["led_on"] if e["led_on"] is not None else 1),
            pin(e["battery"]), int(round((e["adc_mult"] or 0) * 1000)), pin(e["adc_ctrl"]),
            int(e["adc_ctrl_on"] if e["adc_ctrl_on"] is not None else 1)))
        lines.append("    %s," % c_pins(e["other_pins"], 24))
        lines.append("  },")
    lines += ["};", "", "static const int BOARD_LIBRARY_COUNT = sizeof(BOARD_LIBRARY) / sizeof(BOARD_LIBRARY[0]);", ""]
    with open(path, "w") as f:
        f.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--meshtastic", required=True)
    ap.add_argument("--chip", default="esp32s3")
    ap.add_argument("--framework", required=True)
    ap.add_argument("--pio-boards", required=True)
    ap.add_argument("--header")
    ap.add_argument("--json")
    ap.add_argument("--log")
    args = ap.parse_args()
    entries, log = generate(args)
    rev = subprocess.run(["git", "-C", args.meshtastic, "log", "-1", "--format=%h %cs"],
                         capture_output=True, text=True).stdout.strip() or "unknown revision"
    if args.header:
        write_header(entries, args.header, "revision " + rev)
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"source": "Meshtastic firmware (GPL-3.0), revision " + rev,
                       "boards": entries}, f, indent=1)
    text = "\n".join(log)
    if args.log:
        with open(args.log, "w") as f:
            f.write(text + "\n")
    print("%d boards, %d notes" % (len(entries), len(log)), file=sys.stderr)


if __name__ == "__main__":
    main()
