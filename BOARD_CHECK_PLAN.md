# Board check: plan

Read with [CORE_PRINCIPLES.md](CORE_PRINCIPLES.md) and the workspace
`DESIGN_PRINCIPLES.md`.

Written 2026-11-03, from James's questions that day. This is a plan only:
nothing is built yet.

## Why

James can't test boards he doesn't own, so he can't say "here's the RTNode
firmware for X". The board check lets a user with the board find that out for
us. They flash a small check image from a web page. It identifies the board by
itself where it can, asks the user only for what it can't see, and ends with a
report: what was found, and what the user confirmed.

The report is evidence we can rely on without holding the board. It also gives
us everything needed to write that board's RTNode build: radio type and pins,
power control, screen type and orientation, buttons.

## What the user sees

A page beside the web flasher, `docs/board-check/`, built like the Expansion
Kit test page. James asked for it to be as simple as possible for the user: one
instruction at a time, and nothing to type unless detection fails.

1. **"Plug in your board and click Start."** The page asks for the port and
   writes the check image (about a minute). The page says plainly that this
   replaces the firmware on the board, and that RTNode or the old firmware can
   be put back afterwards.
2. **"Looking at your board…"** Automatic detection runs, with no user input
   (see the next section). The page then says either "This looks like a Heltec
   WiFi LoRa 32 V3", or "We found an SX1262 radio but couldn't tell exactly
   which board this is".
3. **"Press the reset button."** The page records every button pressed while
   it waits, so a user who presses the wrong button first still tells us about
   that button. When the board restarts, the page reconnects by itself and
   says "Got it".
4. **"Now press each of the other buttons, one at a time. Click Done when
   you've pressed them all."** The page lists each one as it's found ("Button
   1 found, Button 2 found"). The BOOT button turns up here too.
5. **"Press the X on the screen."** Only when a screen was found, or the
   matched board has one. A touch shows the screen is touch-sensitive and that
   its touch chip answers. Two page buttons cover the other cases: "There's no
   X on my screen" and "Pressing it does nothing".
6. **"Press the screen or any button until the arrow points up and you can
   read UP."** Each press turns the picture a quarter turn. After four turns
   it shows the mirrored versions. The word UP is there because an arrow alone
   looks the same mirrored; a mirrored UP doesn't. The user clicks "It points
   up" on the page.
7. **"Is the background black or white?"** Colour screens only (two big
   buttons). This tells us whether the colours are inverted. A mono OLED is
   always white on black, so the question is skipped.
8. **"What's the name printed on the board?"** Only if detection found more
   than one possible board, or none.
9. **The report**, with "Copy" and "Send it to us" (see The report).

What changed from James's script, and why:

- **The radio check doesn't transmit.** A transmission only proves the radio's
  control lines work, and a short timed listen proves the same thing (below).
  Transmitting would also need an antenna question and a "which country"
  question, to pick a legal frequency. What no software on the board can prove
  is the radio's real output, its amplifier and its range. That's confirmed
  later, when the board runs RTNode and another node hears it.
- **The X shows that touch works, but not how touch lines up with the
  picture.** One touch point can't separate all eight ways a touch panel can
  be turned or mirrored; two can. RTNode doesn't use touch, so step 5 stays
  one press. If touch is ever wanted, the X moves to a second corner for a
  second press.
- **The colour question** (step 7) isn't in the script, but it's one click and
  only appears on colour screens. Without it, a colour screen could come out
  inverted.

## Finding the board by itself

### What's known before the check image runs

The flasher already reads the chip model, flash size and PSRAM size (none,
2 MB or 8 MB) while it writes the image. Web Serial also gives the USB vendor
and product ID, which shows how the board connects:

- native ESP32-S3 USB: Espressif, `0x303A`;
- a Silicon Labs bridge chip: `0x10C4`;
- a WCH bridge chip (CH340, CH9102): `0x1A86`.

These narrow the candidates before any pin is touched. For example, the
Heltec V3 has no PSRAM and a Silicon Labs bridge, while the V4 has 2 MB PSRAM
and native USB.

### The board library

A table compiled into the check image, generated from two sources:

- **Meshtastic's board definitions** (GPL-3, like RTNode). Meshtastic's
  `variants` folder has 68 ESP32-S3 boards, 30 classic ESP32 boards and 9 of
  the smaller C3 and C6 chips, each with a `variant.h` of pin definitions.
- **RTNode's own board table**: 20 boards inherited from the RNode firmware,
  plus the Heltec V4, V4 R8 and the Expansion Kit.

Some Meshtastic definitions use names from the Arduino board support files
(`SDA_OLED`, `Vext`, `RST_OLED`). The generator script runs the C preprocessor
over each `variant.h` together with the Arduino pins file that board's build
uses, then dumps the macros, so every name becomes a pin number. It writes one
entry per board:

- radio types and pins;
- power control (a power switch pin and its "on" level, or a power chip with
  its type and I2C pins);
- screen type, bus and pins;
- touch chip and address;
- buttons, LED and battery pin;
- chip, flash, PSRAM and USB type.

The table is credited to the Meshtastic project on the page and in the
notices. It's refreshed when we choose, not automatically.

Some boards list several radio types, because Meshtastic tries each one at
start-up. The table keeps all of them, and the probe tries each.

### The probe

Each step is safest first, and the whole automatic part takes a few seconds:

```
candidates = boards in the library matching chip, flash, PSRAM and USB type

# Power first: on many boards nothing answers until its supply is on.
for each distinct power setup among candidates:
    if it uses a power chip: read the chip's ID over I2C (0x34 on T-Beams)
                             and switch on the radio and screen supplies
    if it uses a power switch pin: set it to its "on" level
    (after the radio and screen tries below, switch it off again)

for each distinct I2C pin pair among candidates:
    scan the bus: screen (0x3C/0x3D), power chip, touch chip, other parts

for each distinct radio pin set among candidates:
    reset the radio, wait for BUSY if it has one, then read its ID:
        SX127x: version register reads 0x12
        SX126x: the version string in its registers
        SX128x: firmware version register
        LR11xx: version command
    if it answers:
        check its oscillator started (no crystal error reported)
        listen briefly with a short timeout and check that the
            "timed out" signal arrives on the board's IRQ pin
            (this proves the IRQ pin without transmitting)
        put the radio to sleep

score each candidate by what answered; one clear winner = the board
several equal winners = list them all, and ask the board's name (step 8)
```

The radio ID read happens once per pin set, with a fixed short wait from the
chip's datasheet. There are no retries; a candidate that doesn't answer has
simply failed.

### Safety rules

- Try only pin sets that appear in the library; never sweep pins at random.
- Never touch the pins used by the flash, PSRAM or USB (on the ESP32-S3:
  26–32, 33–37 on boards with 8 MB PSRAM, and 19–20).
- Drive pins as outputs only for the few milliseconds a try takes, then
  return every pin to input.
- Switch on one candidate's power at a time, and switch it off afterwards.
- Look for buttons by reading pins only: inputs with pull-ups, never driven.

### Also checked, without the user

- WiFi: a scan, reporting how many networks were seen and the strongest
  signal. No network names.
- Bluetooth: a short scan, reporting how many devices were seen. No names or
  addresses.
- Battery voltage, if the board has a battery pin.

## The check image

A separate small firmware in the RTNode repository. It shares nothing with
RTNode's routing code and needs no networking stack.

- **Radio: RadioLib** (MIT). It takes pins at run time and covers all four
  radio families. RTNode's own radio drivers have the pins fixed when built,
  so they can't be reused here without the big refactor this check avoids.
- **Screens**: the Adafruit SSD1306, SH110X and ST7789 libraries RTNode already
  uses; all take pins at run time. E-paper boards come later.
- **Power chips**: XPowersLib, already in RTNode.
- **Touch chips**: a few register reads per chip (CHSC6x, FT6x36, CST816,
  GT911). Which chips come first is decided when building.
- One build per chip family. The ESP32-S3 build comes first, then classic
  ESP32. RTNode doesn't target C3 or C6, so the check names those boards but
  goes no further.

### How the page and the board talk

After writing the image, the page reopens the port at 115200 baud. Both sides
then exchange one JSON object per line:

```
board → page   {"event":"boot","reason":"..."}
               {"event":"found","board":"heltec_v3","radio":{...},"screen":{...}}
               {"event":"button","pin":0,"state":"down"}
               {"event":"touch","x":212,"y":31}
page → board   {"cmd":"show_x"}  {"cmd":"rotate"}  {"cmd":"show_arrow"}
```

- **The page does the recording.** It's connected throughout and logs every
  event as it arrives, so nothing has to survive the reset on the board.
- **Reconnecting after reset.** On native-USB boards the port disappears and
  comes back. The browser remembers the permission for that device, so the
  page reopens the port without asking.
- **Download mode.** A reset while the BOOT button is held starts the chip's
  download mode instead. The page recognises that and asks the user to press
  reset again on its own.
- **Which pins can be buttons.** All pins except flash, PSRAM and USB, and
  except those already found to belong to the radio, screen or I2C buses.
  That stops the radio's IRQ line from turning up as a button. A T-Beam's
  power key is read through its power chip.

## The report

Shown on the page as a short list:

- the board, as detected or as named by the user;
- chip, flash and PSRAM;
- the radio's type and pins, with its ID, oscillator and IRQ results;
- power control;
- the screen's type, orientation and colours, and whether touch works;
- buttons, by pin;
- WiFi and Bluetooth counts;
- battery voltage.

It contains no MAC address, no network or device names, and nothing the user
typed except a board name.

- **Copy** puts it on the clipboard as text.
- **Send it to us** opens a pre-filled GitHub issue on the RTNode repository.
  The user sees it before submitting, and needs a GitHub account.

How a report gets used:

1. If it matches a library board and everything answered, that's the board's
   evidence. We write its RTNode build from the report's pins, cross-checked
   against the library.
2. The build goes onto a test page, as the Expansion Kit build did.
3. The user installs it. Once another node hears it, the board is "confirmed
   by a user", with the date.

Later the same checks could live in RTNode's own setup portal, so each new
release can be re-confirmed the same way. That's separate work.

## How it fits the design principles

- The check is a separate tool, not part of the node. Each step is bounded:
  every candidate is tried once, with a fixed wait from its datasheet, and a
  candidate that doesn't answer has failed. There are no retries, and no
  timeouts used as a fix.
- Nothing in it comes from Python RNS. It has no networking at all. The
  libraries are MIT or BSD, and the Meshtastic table is GPL-3 with credit, all
  of which fit RTNode's GPL-3.

## Tests

- **Detection, automated:** run the check image on the M5's V4.2 and V4 R8
  over SSH. It must name each board correctly with no user input, and report
  the right radio pins, power control and screen.
- **The generator:** every ESP32-S3 entry it produces for a board RTNode
  already supports must match RTNode's own pins for that board.
- **The page:** drive its steps against a simulated board, a script speaking
  the JSON protocol, in the built-in browser.
- **One real run by a person with a board in hand** for the reset, button,
  touch and arrow steps. Nobody presses buttons on the M5, so this needs James
  with a board at his desk, or the Expansion Kit user.

## Order of work and effort

| Step | Effort |
|---|---|
| The board library generator and table (ESP32-S3 and classic ESP32) | 1–2 days |
| The ESP32-S3 check image: probe, buttons, touch, screen commands, JSON | 3–5 days |
| The page: write, reconnect across reset, the steps, the report | 2–3 days |
| Bench runs and fixes | 1–2 days |
| The classic ESP32 image | 1–2 days more |

That's about two weeks for ESP32-S3 boards, plus a few days for classic ESP32.

## Unknowns

- Whether every Meshtastic definition resolves cleanly through the
  preprocessor. Some pick parts at run time, such as the Heltec V4's two
  amplifier chips. The generator logs what it can't resolve, rather than
  guessing.
- Boards whose screen sits behind a power switch the library doesn't know
  about.
- E-paper screens: the arrow step works, but each turn takes a second or two.
- Analog button ladders, where several buttons share one pin. The check can
  report changing levels on that pin, but naming each button needs more work.

## Decisions for James

1. **Where reports go.** A pre-filled GitHub issue (needs a GitHub account),
   "copy and paste it to us" only, or both. The recommendation is both.
2. **What the first stage covers.** ESP32-S3 boards only (68 Meshtastic boards
   plus ours), with classic ESP32 next. That's the recommendation.
