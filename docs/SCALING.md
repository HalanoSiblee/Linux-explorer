# Scaling

How Linux 2000 makes a desktop bigger on a dense panel, why it does it
the way it does, and how that compares with Windows and the other
desktops. Two mechanisms exist; both are set from Display Properties >
Settings.

## Screen scaling (the plain way)

xrandr builds a smaller virtual screen and stretches it over the panel
(`--scale`). Nothing in the desktop knows: every program draws at 96 dpi
into the small screen and the GPU enlarges the result. This is what a
compositor does for "legacy" programs, and what GNOME on X11 offers as
its only fractional option. At **200%** the nearest filter is used and
every pixel becomes an exact two-by-two block, which is pixel-perfect;
at 125, 150 and 175% no filter can be: nearest gives uneven pixels
(some doubled, some not) and bilinear gives a slight blur. The desktop
picks bilinear there, having tried the other.

## Sharp scaling (supersampled)

The third choice, "Sharp", is how GNOME's mutter and the Wayland
compositors produce a fractional scale: render at the next whole scale
and shrink. The desktop draws itself at 200%, where everything is an
exact pixel doubling and nothing is invented, and xrandr's transform
makes the panel show that virtual screen *smaller* by 200/wanted
(`--scale 1.3333` for 150%). Downscaling with bilinear averages real
pixels rather than inventing them, so edges and text stay crisp where
upscaling from 100% smears them. The cost is a virtual screen a third
larger than the panel for the GPU to render, and fonts hinted at 200%
then shrunk rather than hinted at 150%. It uses the desktop-scaling
machinery below, at its one exact setting. Each monitor keeps its own
scale: a second monitor wanting 100% gets `--scale 2` from the same
200% desktop.

### Sharp 2x

"Sharp 2x" is Sharp taken to its conclusion: the desktop renders at
twice the largest scale any monitor wants -- 300% for a 150% monitor --
and xrandr halves it. At exactly 2:1 xrandr's bilinear filter is a clean
2x2 box: every screen pixel is the average of four rendered ones, so
text and edges come out supersampled rather than stretched, with no
ringing and nothing invented. It is the best picture X11 can be made to
give a fractional scale, and the dearest: four times the pixels of the
scale itself, drawn by every program (GTK and Qt render at 3x). Monitors
at other scales are shrunk by more than two -- a 100% monitor beside a
150% one is shown at 3:1, which bilinear does less cleanly -- so it
suits a desktop whose monitors share a scale. Like Sharp it takes
effect at the next logon. `ScaleMode=supersample2`.

## The nested compositor (experimental)

Everything above ends at xrandr, which stretches a scaled monitor with
one of two filters, nearest or bilinear, and lets nothing else in
between. The one way past it is to move the desktop out of the real
server altogether: with **Experimental: scale the whole picture through
the nested compositor** ticked on the Settings page (`Compositor=nested`
in the scheme), l2k-session starts a nested X server at each monitor's
*logical* size -- 2560x1707 for a 3840x2560 monitor at 150% -- runs the
desktop in it at 100%, and starts `l2kscaler` on the real server.
l2kscaler shows the nested screen on the real one, a window per
monitor, drawn by the GPU with mpv's EWA Lanczos-sharp: a polar jinc
filter, blurred a hair, with a light anti-ringing clamp -- the filter
high-quality video players use, which keeps edges and text clean where
bilinear smears them. Monitors at 100% are copied through unchanged.
Input on those windows goes back into the nested server with XTest at
the matching logical position, and the nested pointer is drawn by
l2kscaler, scaled, since the real one is hidden. One window waits for
the vertical blank per frame; the others do not, so three monitors do
not mean a third of the frame rate.

The nested server is Xephyr when it is installed, started with glamor:
it draws with the GPU and gives its programs hardware GL, so the desktop
inside is as quick as the plain one. Without Xephyr it is Xvfb, where
every program renders in software -- fine for a desktop, not for 3D.
The screen is read out of the nested server through shared memory. With
Xephyr the scaler can instead bind the nested window's own pixmap into
its texture on the GPU (`CompositorCapture=pixmap` in the scheme; no copy
at all), but some drivers take that badly, so it is off unless asked.

The filter works in gamma space, which is what text weight is drawn for;
`CompositorLight=linear` filters in sigmoidised linear light as mpv does,
truer for photographs but thin dark text comes out lighter.

Inside the nested session Display Properties lists the real monitors
(the session tells the desktop where they are, `W2K_MONITORS`), and
scale, primary and enabled can be changed for the next logon; the
layout is fixed while the session runs. It needs Xephyr or Xvfb and
l2kscaler, which the installer provides -- without them the session
starts plainly and says so in `~/.w2k/session.log`. Turn the box off to
go back at the next logon.

To try the scaler without logging off: `l2kscaler --nested :0 --layout
"test,600,400,100,100,400,267,0,0" --window --no-input` shows a piece of
the running desktop scaled 1.5x in a window. Do not run it against a
display that is already being shown through l2kscaler.

## Desktop scaling (the experimental way)

The panel stays at its native size and the desktop renders larger. This
is how Windows draws a DPI-aware program, how macOS draws at 2x, and
what Wayland's `fractional-scale-v1` asks of a client.

### The model

Every program lays out in *logical* pixels -- the Windows 2000 metrics
the whole desktop was measured in: an 18-pixel caption, a 16 by 14
caption button, a 75-pixel icon cell, Tahoma 8 -- and the toolkit
multiplies on the way to the screen:

- `w2k_px(v)` maps a logical coordinate to the screen, `w2k_lp(v)` maps
  back. Windows, menus, tooltips and popups are created at the mapped
  size; pointer positions come back through `w2k_lp()` before a program
  sees them.
- A rectangle's span is mapped as the difference of its mapped ends
  (`w2k_cw`), never as a rounded width, so neighbouring rectangles stay
  neighbours at any scale.
- Fonts are opened at their pixel size times the scale; `w2k_font_height`
  and `w2k_text_width` report logical values, so layout code is unchanged.
- Icons are resampled from the 32-pixel art to the size on screen (the
  16-pixel icon at 200% *is* the 32-pixel art). The XP and Windows 7
  chrome is enlarged from its sheets by nearest neighbour into a copy
  whose columns are the exact inverse of `w2k_px()`, so a piece cut from
  the sheet lands on the pixels it names; a one-pixel column tiled across
  a caption stays one column.
- The mouse pointer is enlarged from the cursor art: blocks at 200%,
  bilinear at fractions.

### Resampling

What the desktop enlarges itself -- icons from their 32-pixel art, the
XP and 7 chrome sheets, the pointer, the wallpaper, Imaging's picture --
goes through a resampler of the user's choice (Display Properties >
Settings > Resampling; on a screen-scaled monitor xrandr has only two
filters, so Nearest there is xrandr's nearest and every other choice is
its bilinear): Lanczos-3, a Catmull-Rom cubic spline (the
default), bilinear, or nearest neighbour. It is separable and
phase-correct, works in premultiplied colour so transparent edges do not
bleed, and widens its kernel when shrinking. A whole-number enlargement
is always exact blocks whatever is chosen. This is the one place a
better filter than bilinear can be had on X11: the X server's own screen
transform offers only nearest and bilinear (its "good" and "best" are
bilinear, and its convolution filter is a fixed kernel with no
sub-pixel phase), so the choice applies to the pictures the desktop
draws, not to the Screen method's stretch.

### Lines

The hard part of a fractional scale is a line. A one-pixel line at 150%
is a pixel and a half; stretching the grid makes it one pixel here and
two there, which is exactly the uneven look screen scaling has. Windows
avoids it because a DPI-aware program lays out in *device* pixels with
scaled *metrics* (`GetSystemMetrics` at 120 dpi says the caption is 24
high) and draws its 3D edges one pixel thick whatever the DPI. Qt does
the same for its cosmetic pens; GTK 3 refused the problem and supports
whole-number scales only.

The desktop follows Windows:

- Every 3D edge, frame and button ring is `w2k_th(1)` thick: one pixel up
  to 199%, two from 200%.
- In a program the rings are anchored to where the control's *inside*
  begins (its rectangle inset by the number of rings), not to its outer
  corner. Two logical pixels can be three on the screen, and anchoring
  outward would leave a coloured gap between the edge and the white of an
  edit box; anchored inward, the spare pixel falls outside, on the
  parent's background, where it is invisible.
- A pushbutton fills its face over the whole rectangle first and draws
  its edge on top, so face and edge always meet.
- Hand-drawn pixel art -- arrows, check marks, the speaker -- keeps the
  stretched mapping, because a triangle built from one-pixel rows must
  stay solid.

### The window manager

The manager's chrome is laid out in screen pixels -- the frame has to fit
the client window it wraps, whose size is whatever the program asked for
-- in a *raw* mode of the primitives where coordinates pass through
unchanged and only thicknesses, fonts, icons and skins scale. Its metrics
(`FRAME_SIZE`, `CAPTION_H`, `CAPBTN_W`) are multiplied once, the way
`GetSystemMetrics` is. The caption glyphs are drawn from their art so
each art pixel covers its own share of the scaled button. The taskbar,
desktop icons, Start panel, balloons and the Alt+Tab box are laid out
logically over physical windows like a program.

### Other programs

Programs the desktop starts are told the scale the way their own
desktops would: `GDK_SCALE` and `GDK_DPI_SCALE` (GTK), `QT_SCALE_FACTOR`
(Qt), `XCURSOR_SIZE`, and `Xft.dpi` in the X resources. GTK 3 renders
whole scales only, so at 150% it draws at 1x with 1.5x fonts, as it does
on any other X11 desktop. The desktop remembers having set `Xft.dpi` and
puts it back to 96 when it next logs on unscaled, so switching scaling
off does not leave other programs large.

### What it does not do

- A change of scale takes a fresh logon: every program opened its fonts
  and sized its windows against the old one.
- Snipping Tool captures are in screen pixels.
- Bitmap wallpapers are not scaled; they are fitted to the monitor as
  before.
