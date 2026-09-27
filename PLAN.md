# Plan

## Done

`lut:color-lookup` (README): .cube (Adobe and Resolve), .3dl (Flame and
Lustre), Hald CLUTs; tetrahedral and trilinear; strength; encoding and
color space; clamp or extrapolate; cache; clear errors in the dialog.
Tested with tests/check.sh (205 checks, also under ASan and UBSan),
tests/gimp-check.sh (GIMP 3.2.6 Flatpak, XCF, command line),
tests/crosscheck.sh (FFmpeg, largest difference 1.8e-7), and looked at in
GIMP's real dialog on Broadway (docs/*.png).

## Next

1. Offer it to GEGL as `gegl:color-lookup` (operations/common, LGPL-3+):
   rename, drop the `lut:` namespace, add a reference composition for
   GEGL's tests.
2. A GIMP MR mapping the PSD Color Lookup adjustment (`clrL`) to it, for
   issue #15505. Photoshop embeds the LUT data in the PSD, so the import
   has to write it to a file next to the XCF, or the operation needs a
   property that holds the table itself; to be decided with upstream.
3. Formats Photoshop also takes: `.look` (Adobe, XML) and abstract or
   device link ICC profiles (babl or lcms). Other LUT formats (`.csp`,
   `.m3d`, `.dat`, `.clf`) only if people ask.
4. More LUT color spaces (Rec. 2020, ACEScg) if a clean babl choice
   exists; Rec. 709 with BT.1886 is not a babl space.
5. Show the LUT's title and size in the dialog.

## Found along the way (to report upstream)

- GEGL `operations/external/png-load.c`: `read_fn` ignores a short read
  from `g_input_stream_read_all` and never calls `png_error`, so a PNG
  that is cut short makes `gegl in.png -o out.png` loop for ever
  (libpng repeats "CRC error" about 200 000 times a second). The fix is
  a `png_error` when `bytes_read < length`. This operation checks PNG
  files itself before loading them.
- GEGL `operations/external/tiff-load.c`: strips that cannot be read are
  left as zeros, with no warning (only a g_message about the ICC tag), so
  a TIFF cut in half loads as a full size image. This operation checks
  that the strips and tiles lie within the file.
- GIMP 3.2 `app/propgui/gimppropgui.c`: a string property with the
  `"error"` key is shown as a message box bound to be visible only with
  text, but `gimp_prop_widget_new_from_pspec` ends with
  `gtk_widget_set_visible (widget, TRUE)`, so the box shows empty until
  the text first changes. Worked around here with a hidden boolean as the
  box's `"visible"` key.
- Setting an operation's own property in `prepare` deadlocks GEGL 0.4.72:
  `gegl_graph_prepare` holds the node's mutex and the notification
  prepares the graph again. The error message is set from the main loop
  instead.
- libgimp caches a filter's settings on the first
  `gimp_drawable_filter_get_config`, and `update` pushes all of them back,
  so a plug-in cannot read settings that the operation changed.
- XCF stores the numbers of a filter's settings as 32 bit floats
  (strength 0.6 comes back as 0.6000000238).
- FFmpeg reads a 16 bit PNG Hald CLUT with differences of up to 5.3e-4
  from its exact values (seen in tests/crosscheck.py); its .3dl reader
  assumes 17 points and divides by 4096, not 4095.
