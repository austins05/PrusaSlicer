# Custom PrusaSlicer Features

This branch adds experimental FFF features on top of PrusaSlicer 2.9.5-beta2. The defaults are intended to preserve normal PrusaSlicer behavior unless a custom option is enabled.

## Sequential Printing With Wipe Towers

Sequential printing can generate a separate wipe tower for each printed object or instance. The wipe tower footprint is compact by default for sequential jobs, with a practical small tower size supported through the existing wipe tower width and depth settings.

Arrangement support can treat each part and its wipe tower as one combined footprint so the arranger keeps both on the bed and avoids placing towers inside neighboring parts. The wipe tower arrange option is in the wipe tower controls.

The sequential wipe tower generation also initializes wipe tower filament data for every tool used by the print. This prevents crashes when a project or CLI profile references a tool index that is higher than the number of explicitly listed nozzle entries.

## Custom Sequential Print Order

Model instances have a numeric sequential print order field. When complete objects is enabled, the slicer uses that numeric order instead of relying on placement order or object height.

The sequential collision check uses the same order so warnings are based on the actual planned print sequence.

## Brick Layer Perimeters

Brick layers are available through the staggered perimeter settings. The implementation staggers eligible inner perimeter paths in Z so adjacent perimeter bands can interlock.

The custom flow controls allow the inner brick-layer perimeter extrusion multiplier to be tuned separately from the outer wall count. This makes it possible to keep the visible outer walls normal while increasing internal brick-layer bonding flow.

When brick layers are enabled with Arachne and the configured perimeter count is not higher than the normal outer wall count, slicing automatically requests one additional perimeter so there is a protected normal outer wall and a real inner brick wall. Open or partial Arachne helper paths are never staggered as brick paths.

If brick layers are enabled with the classic perimeter generator, PrusaSlicer shows a validation warning because classic perimeters do not generate brick-layer paths.

## Experimental Seam Cleanup

All seam cleanup settings default to off and are under print settings in the perimeter advanced controls.

### External Inward Scarf Exit

This option applies only to eligible external perimeter closed loops. It tapers extrusion near the end of the loop, moves the nozzle inward into the wall, then optionally retracts after the inward move.

Settings:

- External inward scarf exit
- Scarf taper length
- Scarf end flow
- Inward exit distance
- Retract after inward exit

The intent is to pull the seam blob inward instead of wiping sideways over the visible outside surface.

### Internal Brick Seam Tuck

This option applies only to eligible internal brick-layer closed loops. It overlaps past the seam, optionally dips Z, wipes inward into the internal region with reduced flow, retracts, then restores Z.

Settings:

- Internal brick seam tuck
- Overlap past seam
- Z dip
- Inward wipe distance
- Wipe extrusion flow
- Retract after tuck

The intent is to hide seam buildup inside the part where the brick-layer geometry is already internal.

## Experimental ZAA Surface Contouring

ZAA surface contouring is an off-by-default experimental option adapted from the BambuStudio-ZAA / OrcaSlicer idea. It varies Z along eligible top solid infill and ironing extrusion paths so the nozzle can follow shallow model surfaces instead of printing every surface as a flat staircase.

Settings:

- ZAA surface contouring
- ZAA minimum layer thickness
- ZAA sample spacing
- ZAA max segment Z change
- Disable ZAA in this region

Implementation notes:

- The slicer uses a lower slice plane while ZAA is enabled so top-surface infill exists close enough to the mesh surface to contour safely.
- Extrusion paths remain 2D internally with a per-point Z-offset sidecar, avoiding the broader 3D path rewrite used by the original ZAA branch.
- G-code output uses existing variable-Z extrusion support and bypasses arc fitting only for contoured paths.
- Safety checks keep the feature limited to solid top-like regions and ironing, require mostly valid upward-facing mesh hits, clamp segment-to-segment Z changes, skip bridges/supports/wipe tower paths, and leave paths planar when checks fail.
- Output is unchanged when ZAA is disabled.

## Experimental Z Stitching

Z stitching is an off-by-default experimental print setting that varies Z up and down along eligible internal extrusion paths. The goal is to mechanically stitch adjacent layers together instead of depositing every internal line as a perfectly planar bead.

Settings:

- Z stitching
- Z stitching amplitude
- Z stitching spacing
- Z stitching minimum foundation

Safety behavior:

- Z stitching amplitude is capped at +/-0.1 mm.
- Z stitching peak spacing cannot be below 1.25 mm.
- Stitching is disabled until enough material has already been printed, and the actual stitched nozzle Z is not allowed below the configured minimum foundation height.
- External perimeters, visible top paths, ironing, bridges, supports, skirt/brim, wipe tower paths, and paths already using ZAA offsets are skipped.
- Z stitching does not scale extrusion flow with the Z offset. That is intentional: unlike ZAA, this is a mechanical path modulation rather than a variable layer-thickness compensation.
- Output is unchanged when Z stitching is disabled.

## Tullomer Profile Work

The local configuration includes a Tullomer filament profile adapted from the available Tullomer guidance and the Qidi Plus 4 high-temperature setup. The active 0.02 mm Tullomer test preset enables Z stitching at 0.1 mm amplitude, 1.25 mm spacing, and a 0.35 mm minimum foundation height. That profile work lives in the local PrusaSlicer configuration rather than this source tree.

## Bambu Lab X1 Carbon LAN Host

The Bambu Lab vendor bundle adds an experimental Bambu Lab X1 Carbon 0.4 mm printer profile. Its default host type is Bambu Lab LAN.

Physical printer setup:

- Hostname/IP: X1C LAN IP address
- API key: Bambu LAN access code
- Username: X1C serial/device ID

The LAN backend uses FTPS to upload to the printer SD card with the standard `bblp` user. Plain Upload sends the generated G-code file to the SD card. Upload and Print wraps the generated G-code into a minimal `.gcode.3mf` package containing `Metadata/plate_1.gcode`, uploads that package, then sends a local MQTT `project_file` start command to `device/<serial>/request`.

When sending to a Bambu Lab LAN host, the upload dialog exposes Bambu print-start options:

- Use AMS
- Optional raw AMS mapping array, for example `[0,1,2,3]`
- Build plate type
- Bed leveling, flow calibration, vibration calibration, first layer inspection, and timelapse toggles

The Bambu Lab LAN host supports network lookup through the physical-printer Browse button using the Bambu mDNS service.

The Window menu also includes `Bambu Lab LAN Control` for the selected physical printer. It can request the printer status report over MQTT, show a parsed status summary, pause/resume/stop an active print, set nozzle/bed/chamber temperatures, set the printer speed mode, send a single raw G-code line, set camera recording/timelapse flags, list SD-card files over FTPS, start a selected SD-card file, and delete selected SD-card files.

Current limitations:

- This is local LAN support only, not Bambu cloud login or device-account sync.
- AMS support is a local print-start mapping field, not the full Orca/Bambu Studio spool sync and AMS material matching UI.
- Camera streaming, HMS/error explanation panels, cloud account binding, remote device sync, and full AMS material matching panels are not implemented yet.

## Current Notes

These features are experimental. For production prints, test with simple geometry first, inspect preview and G-code around seams or wipe towers, and keep the custom options disabled for jobs that do not need them.
