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

## Tullomer Profile Work

The local configuration includes a Tullomer filament profile adapted from the available Tullomer guidance and the Qidi Plus 4 high-temperature setup. That profile work lives in the local PrusaSlicer configuration rather than this source tree.

## Current Notes

These features are experimental. For production prints, test with simple geometry first, inspect preview and G-code around seams or wipe towers, and keep the custom options disabled for jobs that do not need them.
