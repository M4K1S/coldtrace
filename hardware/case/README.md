# ColdTrace case

Two-part enclosure (base and lid), designed in Autodesk Fusion.

![Case: base and lid](preview.png)

## Files

| File | Use |
|---|---|
| `coldtrace_case.f3d` | Fusion design file. Open this to edit the case. |
| `coldtrace_case.step` | CAD exchange format (both parts). Opens in any CAD program. |
| `stl/case_base.stl` | Base, ready to slice. |
| `stl/case_lid.stl` | Lid, ready to slice. |

## Size

Outer dimensions, from the STL files:

| Part | Width | Depth | Height |
|---|---|---|---|
| Base | 92 mm | 100.5 mm | 39 mm |
| Lid | 92 mm | 100.5 mm | 4 mm |

## Hardware (not printed)

| Part | Quantity | Notes |
|---|---|---|
| M3 heat-set inserts | 4 | One per corner of the base. |
| M3 × 4 mm screws | 4 | Hold the lid to the base. The heads sit in 3 mm deep counterbores in the lid, which suits socket-head cap screws (an M3 head is 3 mm tall). |
| Round magnet | 1 | About 40 mm diameter; a smaller one also works. A rubber-coated magnet is best, because the coating stops the case slipping on the surface it's mounted to. |

## Printing

The STL files are in millimetres with Z up, and each part sits on the bed the way it is shown in the preview image:

- **Base:** floor on the bed, open side up.
- **Lid:** flat face on the bed, recessed side up.
