# Audit, September 2026

A pass over the whole tree (37,000 lines across the toolkit, the window
manager and the programs) looking for bugs, rendering faults and slow
paths, done on 5 September 2026 against 1.10.2. What was run, what was
found, and what changed. A [second pass](#second-pass-8-september-2026),
over the scaling code and over the speed and memory of the whole tree,
follows it.

## What was run

- **Every source file compiled with a wide warning set** (`-Wall -Wextra
  -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes
  -Wformat=2 -Wvla -Wnull-dereference -Wduplicated-cond -Wlogical-op`):
  16 warnings, all shadowed locals, unused parameters and missing
  prototypes; none a defect.
- **GCC 14's static analyzer** (`-fanalyzer`) over every file: 120 reports,
  60 of them possible `snprintf` truncation (bounded, benign), and the
  rest listed below.
- **AddressSanitizer and UndefinedBehaviorSanitizer** builds of the whole
  tree, driven through every render harness (window frame, taskbar, both
  Start menus, the balloon, Explorer, Control Panel and each of its
  applets, Network and Dial-up Connections, all four Display Properties
  pages, Notepad, Calculator, Task Manager, Character Map, Device Manager,
  Imaging, Snipping Tool, About Linux 2000 and Linux 2000 Update) in the
  classic, Windows XP and Windows Classic Dark looks: no reports.
- **Fuzzing** of the PNG, JPEG, BMP and ICO decoders (1,200 mutations of
  each sample) and the .cur decoder (1,500 mutations of two cursors)
  under the sanitizers: no reports.
- **Leak checking** of the window manager, Explorer, Control Panel and
  Notepad harness runs: the only leaks are fontconfig's own one-time
  allocations; nothing of ours leaks per event.
- **A visual sweep**: contact sheets of all 26 harness renders in each
  of the three looks, read for mispaints.
- **Live tests** of the Snipping Tool's save paths with synthetic input
  (done for 1.10.2 and repeated here).

## Bugs found and fixed

| Where | What | Fix |
|---|---|---|
| `lib/png.c` | A palette PNG whose PLTE chunk is missing or short read colours from an uninitialised palette table. | The table starts zeroed, so such pixels come out black rather than as stack garbage. |
| `wm/startpanel.c` | The Luna Start panel wrote through `push()`'s return value without checking it; a full row table would have been a null write. | Every use checks the row. |
| `lib/cursor.c` | `w2k_cursors_init()` dereferenced the slot for every cursor role; a role without a slot would crash. | The slot is checked. |
| `lib/list.c` | `tree_next_visible()` dereferenced its argument before its own null check. | Null is handled first. |
| `apps/l2kcontrol.c` | Default Programs focused its first edit box even with no boxes. | Guarded. |
| `lib/assoc.c` | The `.desktop` name handed to `xdg-mime` was placed inside single quotes by hand; a quote in the name would have broken out. | It goes through `w2k_shell_quote()`. |
| `lib/edit.c`, `lib/list.c` | The growable text and item buffers assigned `realloc()`'s result straight back, so a failed allocation lost the old block and then dereferenced null. | The new block is checked and the old one kept on failure. |
| `apps/l2kdm.c` | The typed password stayed in the logon box's memory for the whole session after PAM had used it. | `w2k_edit_wipe()` scrubs the box as soon as authentication returns. |
| `wm/wm.c` | If the notification service's D-Bus descriptor went bad, `select()` failed with `EBADF` and the main loop treated it as fatal: the desktop would have logged off. | `EBADF` closes the service and carries on; only other errors end the loop. |
| `apps/l2kupdate.c` | Links and the "updates only when you ask" note were dark blue and dark grey on the Windows Classic Dark window colour, so they vanished. | Links lighten on a dark window, as the folder windows' do; the note uses the scheme's grey. |

## Performance

| Where | What | Change |
|---|---|---|
| `lib/list.c` | Icon-view labels were wrapped by measuring every prefix of the name, so a folder of a hundred long names cost thousands of text measurements on every repaint and scroll. | Measured at the spaces only, with a bisection for a single long word. |
| `lib/draw.c` | `w2k_ellipsis()` shortened a string one character at a time, measuring each time; a report view of long names paid for it on every row it drew. | The longest fitting prefix is found by bisection. |

Checked and left alone: the window manager sleeps in `select()` until X
or a due timer (clock, tooltip, balloon) needs it, so an idle desktop
wakes once a minute; the list views draw only the visible rows; Explorer
does one `lstat` per entry and looks icons up through a cache; the
Snipping Tool dims a 4K screen with a word-at-a-time pass rather than
per-pixel calls; the Xft draw surfaces and text faces are cached; the
taskbar keeps its back buffer between repaints; the wallpaper is rendered
once per change.

## Rendering

The sweep found one fault (the update page on the dark scheme, above).
Everything else — frames, menus, both Start menus, dialogs, the folder
windows, the balloon — painted correctly in all three looks.

## Not changed

- 60 `-Wformat-truncation` notes: every one is a bounded `snprintf` into
  a buffer sized for the normal case, where truncation loses nothing that
  matters (a display label, a path far beyond `PATH_MAX`).
- The shadowed-variable and unused-parameter warnings: cosmetic.
- The `system()` and `popen()` calls: each runs a fixed command or one
  built from numbers, an enumerated name, or a value quoted with
  `w2k_shell_quote()`.

## How to repeat it

    # warnings
    for f in lib/*.c wm/*.c apps/*.c; do gcc -std=gnu11 -Wall -Wextra -Wshadow \
        -Iinclude $(pkg-config --cflags xft freetype2 dbus-1) -DHAVE_DBUS \
        -DW2K_VERSION='"x"' -fsyntax-only "$f"; done
    # the analyzer
    ... -fanalyzer -c -o /dev/null "$f"
    # sanitizers: build a copy of the tree with
    make CFLAGS="-std=gnu11 -O1 -g -fsanitize=address,undefined -Iinclude \
        $(pkg-config --cflags xft freetype2 dbus-1)" LDFLAGS="-fsanitize=address,undefined"
    # then run the harnesses with W2K_RENDER=<file.ppm> [W2K_RENDER_DIALOG=...]

# Second pass, 8 September 2026

A second audit against 1.17.2: the scaling code -- every mode, the
switching between them and the experimental desktop scaling -- read end
to end, and the whole tree measured for time and memory. The same tools
as the first pass, plus a comparison of 120 harness pictures (fifteen
looks and resampling settings, two scales, four desktop dialogs)
rendered before and after every change.

## Scaling

Read: `w2k_px`, `w2k_lp`, `w2k_cx`, `w2k_cw`, `w2k_th` and every caller;
the three modes (screen, sharp and desktop) and the scheme keys that
choose them; the logon path that applies a mode; and the Display
Properties page that sets one. The rounding policy holds everywhere it
was checked: logical to physical truncates, physical to logical rounds,
and a span is the difference of its mapped ends, so neighbouring spans
tile without a gap or an overlap at every whole and fractional scale.
The sanitizer build was run through the harnesses at 100%, 150% and
200% in five looks without a report. Two faults, both in switching:

| Where | What | Fix |
|---|---|---|
| `wm/wm.c` | Turning scaling off left `Xft.dpi` in the X resources at the scaled value, so GTK and Qt programs stayed large at the next logon. | The desktop remembers having set it (`~/.w2k/.xft-dpi-set`) and merges `Xft.dpi: 96` back when it logs on at 100%. |
| `apps/l2kdisplay.c` | A scale, method, mode, resolution or refresh-rate change was applied only if the Settings tab was still showing when OK or Apply was pressed; look at another tab first and it was dropped without a word. | The page keeps a Settings-tab flag and applies from whichever tab is up. |

## Performance

Every program was timed from start to first paint under the render
harness and its peak memory taken. Most start in 20-40 ms in 7-13 MB.
What did not, and what changed:

| Where | Before | After |
|---|---|---|
| Desktop paint, 3840x2160, a 1920x1200 wallpaper filled (cubic) | 0.79 s, 305 MB | 0.47 s, 86 MB |
| `w2k_rgba_resample`, 1920x1080 to 3840x2160, cubic | 0.38 s, 262 MB | 0.10 s, 43 MB |
| same, Lanczos | 1.59 s, 262 MB | 0.10 s, 43 MB |
| same, 3840x2160 down to 1920x1200, Lanczos | 1.27 s, 267 MB | 0.07 s, 43 MB |
| Device Manager start | 0.41 s | 0.05 s |

- **`lib/resample.c`** held three floating-point copies of the picture
  and evaluated the kernel -- two sines for Lanczos -- for every tap of
  every pixel. It now works out each axis's tap weights once and streams
  the picture through a ring of across-filtered rows, holding one output
  row's taps at a time. The pixels are the same, to a rounding tie: an
  exact half, which a hard edge sampled at phase 1/2 gives at 150%, now
  always rounds up instead of going with the order of a floating-point
  sum. (The desktop paint's remaining 86 MB is the 4K X image and the
  decoded picture, both freed after the paint.)
- **`lib/device.c`** ran `modinfo` for every device with a driver at
  start-up, though only the properties sheet shows the result, and
  `lspci` once per PCI slot. `modinfo` runs when a sheet opens;
  `lspci` runs once and each device finds its line by address.
- **`lib/draw.c`** decomposed the visual's colour masks -- shift, width,
  maximum for each channel -- inside `w2k_rgb()`, which is called for
  every pixel of every picture. Once per visual now, with an 8-bit fast
  path.
- **`wm/desktop.c`** stored the wallpaper with `XPutPixel`, a call and a
  branch ladder per pixel, eight million of them at 4K. A 32-bit image
  in the host's byte order is written straight through.

## Also fixed

- `lib/device.c`: PCI devices were named with their slot and class
  ("00:03.1 PCI bridge [0604]: ...") because the name was cut at the
  first colon of the domain-qualified address; the name is now what
  follows the class.
- `lib/win.c`: the timer table held eight, and the ninth timer -- the
  Control Panel's file-types page makes one per class for its caret --
  was dropped in silence. Thirty-two.
- `apps/l2kcontrol.c`: a null check the analyzer asked for;
  `apps/l2kswatch.c`: an icon loop bounded by its table;
  `lib/gtkcolors.c`, `include/w2k.h`: prototype and format warnings.

## What was run

- The warning set of the first pass: 17 warnings, all shadowed locals,
  missing prototypes and one non-literal format; the prototypes and the
  format fixed, the rest cosmetic.
- The analyzer: 51 reports. The stack-overflow claims at
  `apps/l2kcontrol.c:565` are bounded by `MAX_SLIDERS` and the fixed
  radio and check tables; the null-dereference claims in
  `lib/folderwin.c` are guarded a line above; the one in
  `apps/l2kcontrol.c` is now checked.
- AddressSanitizer and UndefinedBehaviorSanitizer through 191 harness
  renders -- thirteen programs and four desktop dialogs, in the classic,
  Windows XP, Windows 7, Windows Vista and Modern dark looks at 100%,
  150% and 200% -- before and after the changes: no reports.
- The 120-picture comparison: the only differences are the rounding
  ties in scaled icons at 150% (563 pixels of 92 million) and the clock.

## How to repeat it

    # the resampler, old against new
    gcc -O2 -Iinclude $(pkg-config --cflags xft freetype2) -o rsbench \
        tools/rsbench.c lib/resample.c -lm && ./rsbench 1920 1080 3840 2160 2
    # a desktop paint at 4K with the wallpaper in ~/.w2k/scheme
    W2K_RENDER=desk.ppm W2K_RENDER_DIALOG=desktop W2K_RENDER_W=3840 \
        W2K_RENDER_H=2160 bin/l2kwm
