# Controller bindings

*September 2026.*

Until now a recompiled title got one pad, on port 1, driven by XInput pad 0
and a keyboard map written into `src/hle/input_host.c`. Nothing could be
rebound, a second player could not be plugged in, and the only way to change a
key was to edit C and rebuild every title.

There are now three pieces:

| | |
|---|---|
| `src/input/input_bindings.c` | reads the config, samples the four ports |
| `tools/input_ui/` | the UI that writes the config (`py -3 -m tools.input_ui`) |
| `tools/input_ui/bindings.py` | the schema both sides speak, and its tests |

Every title built from this toolkit picks it up: the binding layer is part of
`xbox_input`, which `xbox_hle` and `xbox_usb` both link, so a title only has
to be rebuilt, not changed.

## The file

JSON, one entry per controller:

```json
{
  "version": 1,
  "pad_api": "sdl",
  "controllers": [
    {
      "port": 1,
      "device": "gamepad:0",
      "deadzone": 7849,
      "bindings": {
        "a": ["pad:a", "key:Z"],
        "start": "key:RETURN",
        "lstick_left": ["pad:lx-", "key:A"],
        "black": "none"
      }
    }
  ]
}
```

Ports are **1-4**, as they are written on the console. A controller the file
does not mention keeps its defaults, and so does a control the file does not
mention -- a file with one binding in it is a config with one binding changed.
That is deliberate: it makes the file hand-editable and a diff readable.

**`device`** is `keyboard`, `gamepad:0` .. `gamepad:3`, or `none`. It says where
`pad:` sources are read from. `key:` sources are always read, whatever the
device is, which is why the default port 1 answers to both a pad and the
keyboard at once. `xinput:0` .. `xinput:3` is what files written before SDL3
say, and names the same slot.

**`pad_api`** (Sep 2026) is which API reads every pad: `"sdl"`, the default, or
`"xinput"`. SDL3 reads Xbox pads and everything else -- DualSense and
DualShock 4, Switch Pro, 8BitDo, the Steam Deck, generic pads through SDL's
mapping database -- with hotplug and rumble; a pad takes the first free slot
when it is connected and keeps it until unplugged. XInput reads Xbox
controllers only, by XInput's user index, and is kept for a pad or machine
where SDL3 misbehaves. `RECOMP_PAD_API=sdl|xinput` overrides the file, and
the launcher's Input tab has it as "Controllers". Both go through
`src/input/recomp_pad.h`, which returns XInput's layout whichever API read
the pad; `recomp_pad_probe [seconds] [xinput]` (built on request, target
`recomp_pad_probe`) prints what it sees, live.

**A binding** is one source string or a list of up to four. Any of them
pressing the control presses it, and the strongest wins for anything analog,
so a trigger bound to both a key and a pad trigger behaves sensibly. `"none"`
or `[]` unbinds.

**Sources**

| form | meaning |
|---|---|
| `key:Z`, `key:7` | a letter or digit |
| `key:RETURN`, `key:LSHIFT`, `key:F8`, `key:SLASH` | a named key (the list is `KEYS[]` in `input_bindings.c`, mirrored in `bindings.py`) |
| `key:#191` | a raw Windows virtual-key code, for anything with no name |
| `pad:a` `pad:b` `pad:x` `pad:y` | face buttons on the host pad |
| `pad:lshoulder` `pad:rshoulder` | shoulder buttons (the Xbox's Black and White by default) |
| `pad:start` `pad:back` `pad:lthumb` `pad:rthumb` | the rest of the digital buttons |
| `pad:dpad_up` .. `pad:dpad_right` | the host D-pad |
| `pad:lt` `pad:rt` | analog triggers, 0-255 |
| `pad:lx+` `pad:lx-` `pad:ly+` .. `pad:ry-` | one half of one stick axis |
| `mouse:left` `mouse:right` `mouse:middle` | mouse buttons, primary and secondary as Windows has them (a left-handed setting carries over) |
| `mouse:x1` `mouse:x2` | the side buttons (back, forward) |
| `mouse:wheel_up` `mouse:wheel_down` | one short press per wheel notch (60 ms down, 40 ms up, up to four queued) |

**Controls** are the 24 inputs an Xbox pad has: `a b x y black white start
back ltrigger rtrigger`, `dpad_up/down/left/right`, `lthumb`, `rthumb`, and
each stick as four halves -- `lstick_up/down/left/right` and `rstick_*`.

A stick is four halves rather than two axes because a key has no middle. It
also means every source, whatever it is, reads as one 0-255 magnitude:
a digital control fires above a quarter press, an analog button takes the
magnitude, and a stick axis is the positive half minus the negative one,
scaled to the signed 16 bits the guest reads. Binding a trigger to a key
gives a full pull; binding a stick half to a key gives a full deflection.

### The mouse

*October 2026.* Two things, both read from raw input (`WM_INPUT`) on the
game window, so only while that window is in front:

- **Mouse buttons are sources**, bound like keys: `"rtrigger": ["pad:rt",
  "mouse:left"]`. The launcher puts them in the keyboard column ("KEYBOARD /
  MOUSE"): click, or turn the wheel, when it asks for a key.
- **Mouse look**: the mouse's movement turns one controller's stick. It is a
  top-level object, because there is one mouse:

```json
"mouse": {
  "stick": "right",
  "port": 1,
  "sensitivity": 1.0,
  "invert_y": false,
  "anti_deadzone": 0.2
}
```

`stick` is `off` (the default), `right` or `left`; `port` is the controller
(1-4) whose stick it is. The mouse's deflection is added to whatever that
stick's own bindings say, and the sum clamped, so a pad and the mouse can both
turn the camera.

| variable | overrides | values |
|---|---|---|
| `RECOMP_MOUSE_STICK` | `stick` | `right`, `left`, `off`; `1`, `on` or empty mean `right`, `0` means `off` |
| `RECOMP_MOUSE_SENS` | `sensitivity` | a number; 1.0 is the default |
| `RECOMP_MOUSE_INVERT_Y` | `invert_y` | `1`/`0`, empty means on |
| `RECOMP_MOUSE_PORT` | `port` | 1-4 |
| `RECOMP_MOUSE_ANTI_DEADZONE` | `anti_deadzone` | 0-0.9 |

The launcher's Input tab has three rows for it, after "Controllers":
**Mouse look** (Off, Moves the right stick, Moves the left stick, for the
controller being edited; turning it on takes the mouse from whichever
controller had it), **Mouse sensitivity** (0.25x to 5x in steps) and **Invert
mouse Y**. `py -3 -m tools.input_ui` has the same, as a "MOUSE LOOK" row
under the device buttons. Both write the `mouse` object above into the
bindings file, so it is per person, not per title, like every other binding.

**How movement becomes a stick** (`src/input/recomp_mouse.c`). A mouse says
how far it moved; a stick says where it is held, and a title turns that into a
turning rate. So the deflection follows the mouse's speed. The window thread
adds raw counts to two interlocked accumulators and never waits; each pad poll
drains them under a small lock and steps a model forward:

- the deflection decays towards centre with a 40 ms time constant;
- the counts since the last poll are added as if spread evenly over the time
  since that poll, which makes a steady speed give a steady deflection
  whatever the title's poll rate -- 300,000 polls a second and one a frame give
  the same answer (measured: 16,211 and 16,332 for the expected 16,384);
- at sensitivity 1.0, 2,500 counts a second holds the stick fully over: a slow
  sweep with an 800 dpi mouse. Faster than that saturates, clamped radially
  (a stick's gate is round), so a flick stops turning when the hand stops;
- below 2% it reads as centred; above, it is lifted to at least
  `anti_deadzone` of the travel, because most titles ignore the first quarter
  of a stick and a slow movement would otherwise vanish inside it.

**The cursor.** While mouse look is on and the window is in front, the cursor
is clipped to the picture and hidden: a cursor that wanders off the window
takes the focus with it at the first click. **F8** lets it go (F9, F10 and F11
are taken by the frame-rate overlay, the frame cap and the frame capture, and
F12 breaks into an attached debugger); a click on the picture, or coming back
with Alt+Tab, takes it again, and that click is not passed to the title.
Leaving the window lets go of the cursor and of every held mouse button.

Nothing of this runs unless the config asks for it: with mouse look off and no
`mouse:` source bound, no raw input is registered and the window's messages
pass through untouched. The window is the D3D8 replacement's
(`src/hle/hle_d3d8.c`), so a run with `RECOMP_HLE_D3D8=off` has no mouse.

```
[INPUT]   controller 1: SDL3 pad 1 + keyboard + mouse
[INPUT] mouse moves controller 1's right stick (sensitivity 1.00, Y not inverted, anti-deadzone 0.20)
[INPUT] mouse captured: F8 lets the cursor go, a click on the picture takes it back
[INPUT] port 1 first press: right stick (mouse) (SDL3 pad 1 + keyboard + mouse)
```

### Where it lives

Both sides look in the same places, in this order:

1. `RECOMP_INPUT_CONFIG`, if set -- an explicit path, and a run says so if it
   cannot be read rather than silently falling back;
2. `%APPDATA%\xboxrecomp\input_bindings.json` (on POSIX,
   `$XDG_CONFIG_HOME/xboxrecomp/input_bindings.json`) -- the per-user file the
   UI writes by default;
3. `input_bindings.json` in the working directory, which for a title started
   by its `run.bat` is the directory the executable sits in, so a title can
   ship a config of its own.

Bindings are per-user rather than per-title on purpose: they belong to the
person and their hardware, and a toolkit that builds several titles should not
make them rebind for each one. A title that genuinely needs its own gets one
through (3), and a test run gets one through (1).

### Defaults, when there is no file

Exactly what the runtime did before this existed, so nothing changes for a run
that has not been configured:

- controller 1: XInput pad 0 **and** the keyboard -- arrows = D-pad,
  Enter = START, Backspace = BACK, Z = A, X = B, A = X, S = Y, Q = White,
  W = Black, E = left trigger, R = right trigger;
- controllers 2-4: XInput pad 1, 2, 3, pad only.

The sticks have no keyboard default. Every comfortable key around WASD is
already a button in that map, and guessing would have silently changed what
existing runs do.

## The UI

```
py -3 -m tools.input_ui                  # the per-user config
py -3 -m tools.input_ui path\to.json     # somewhere else
py -3 -m tools.input_ui --print          # what the runtime would load
py -3 -m tools.input_ui --write-defaults # a default file, no display needed
```

Four controllers down the left as dashboard blades; the selected one's device
across the top -- the keyboard, each XInput slot with whether a pad is
answering right now, or nothing; every control below it. Clicking a binding
(or `Bind`) opens a capture: press a key or a button and it is taken, `+` adds
a second source to the same control, Esc cancels, and the dialog has a button
to clear. `Reset this controller`, `Reset all four`, `Save`.

It is tkinter with every colour set by hand -- black field, `#0D0D0D` panels,
`#107C10` for structure and `#9BE04B` for anything with focus or a value. ttk
on Windows will not let a widget be black, and `tk.Scrollbar` keeps the
system's white trough whatever it is told, so the scrollbar is drawn here too.

Device detection is XInput through `ctypes` (`tools/input_ui/devices.py`): it
tries `xinput1_4`, `xinput1_3`, then `xinput9_1_0`, and asks each of the four
slots. DirectInput and raw HID are not read -- an Xbox-layout pad is what
XInput reports, and anything else would need a translation layer the toolkit
does not have. A pad that is not detected can still be bound and configured;
the port simply reports empty until it is plugged in.

Capture compares against the pad state taken when the dialog opened, so a
stick resting off centre or a trigger resting high does not bind itself.

## What the runtime does with it

`recomp_bindings_init()` runs once, from whichever input path is reached
first, and logs what it read:

```
[INPUT] bindings from C:\Users\...\AppData\Roaming\xboxrecomp\input_bindings.json
[INPUT]   controller 1: keyboard
[INPUT]   controller 2: nothing
[INPUT] XAPI input replaced by name: 1 pad(s); gamepad type 0x0028C054
[INPUT]   port 1 <- keyboard
[INPUT] XInputOpen port 1 (keyboard) -> handle 0x58490001
[INPUT] port 1 first press: start (keyboard)
```

That last line is named once per port, the first time anything is pressed on
it. A wrong binding is otherwise invisible -- the title just does not respond,
and there is no way to tell a mis-bound key from a title that is not reading
its pads at all.

`src/hle/hle_input.c` now reports **up to four ports**. A port is present when
its device is the keyboard, or when its XInput pad answers, or when it has
keyboard bindings as well (which is why the default port 1 is always there).
`XGetDevices` and `XGetDeviceChanges` re-check, so a pad plugged in while the
title runs arrives as an insertion the way the console reported one, and
`XInputGetState` samples the device bound to that handle's own port.

Sources are resolved to key codes and pad indices once, at load, so a poll
does no string work; a poll reads each distinct key once and its pad once. An
empty XInput slot is re-checked only once a second, because `XInputGetState`
on one costs about a millisecond and a title polls several times a frame --
that was true before this change too, and silently cost more than the game on
a machine with no pad.

`RECOMP_FAKE_INPUT` and `RECOMP_INPUT_SEQ` are unchanged and apply to
controller 1 after its bindings. They stand in for a person at the keyboard,
so what that person bound should not change them.

`src/usb/usb_gamepad.c` reports port 1 through the same bindings, so a title
that finds its pad through the emulated USB stack rather than the XAPI
replacement behaves the same.

## Tests

```
py -3 -m unittest discover -s tools -p "test_*.py" -t .
```

`tools/input_ui/test_bindings.py` covers the half that needs no device:
defaults, what a partial or broken file means, which sources are valid, and
that a saved config reloads identical.

`tools/input_ui/test_runtime_vocabulary.py` reads `CONTROLS`, `PAD_BUTTONS`,
`KEYS`, `MOUSE_SOURCES`, `DEFAULTS` and the mouse defaults out of
`src/input/input_bindings.c` (and the launcher's mouse defaults out of
`src/launcher/launcher_bindings.c`) and fails if the Python copies have
drifted. Drift is the dangerous failure here: the UI would
write a name the runtime does not recognise, the file would save, and the
control would simply never fire.

## Verified

Burnout 2, built from this branch:

- with no config file: boots as before, one pad on port 1, the log naming the
  built-in defaults;
- with a config naming `keyboard` on port 1, `none` on port 2 and START bound
  to `key:F8`: the log named the file and the devices, and pressing F8 came
  through as `port 1 first press: start`.

## Not done

- **Hot reload.** The config is read once, at start-up. Re-reading it when the
  file changes would be a few lines, but a binding that changes mid-frame
  wants thinking about.
- **DirectInput and raw HID**, so a PlayStation or generic pad appears without
  a third-party translation layer.
- **Rumble** goes to the model and no further: nothing drives the host pad's
  motors yet, on any port.
- **Per-title overrides** beyond dropping a file next to the executable -- no
  profiles, no per-title directory in the per-user config.
- **A POSIX backend.** The parser is portable and builds anywhere; the
  sampling is XInput and `GetAsyncKeyState`, so on POSIX every port reads
  neutral until an SDL2 backend exists (the same gap `src/hle`'s audio and
  input backends already have).
- **The mouse in a running title.** The stick model and both config readers
  are tested offline; capture, release and raw input on the game window have
  not yet been played with. Off Windows the mouse does nothing.
- **Memory units and other XPP devices.** Only gamepads are bound; the model
  still answers "nothing connected" for everything else.
