![image](https://user-images.githubusercontent.com/106916061/179006347-497d24c0-9bd6-45b7-8c49-d5cc8ecfe5d7.png)
# BambuStudio ON STEROIDS

A fork of Bambu Studio (based on the 2.8.3 beta) with a built-in thermal simulation, a settings advisor, and a batch of slicing, G-code and stability fixes. Everything runs offline on your computer. No account, no cloud, no subscription.

## What is different from the original

### Thermal Optimization (new)
A thermometer button in the top bar, available once a plate is sliced.
- **Analyze** simulates how warm each layer still is when the next one lands and rates every toolpath from cold (-1) to hot (+1). The result shows up as the Thermal Index colour schemes in the preview, with an outcome, the direction of the problem (overcooling or overheating), per-layer caveats and concrete suggestions.
- **Optimize** rescales layer speeds so every layer lands inside the material's comfort window, within your limits (speed-up, slow-down, volumetric cap, keep outer walls). Layer 0 and quality-sensitive roles are left alone. M73 progress lines are recomputed, so the printer's time estimate stays right.
- **Material aware**: one model per filament slot from the filament presets (PLA, PETG, ABS, ASA, PC, PA, PAHT, PPA, PPS, PP, PE, TPU, PVA, PEEK, PEI and their CF/GF variants), with the preset's vitrification temperature, maximum volumetric speed and slow-down minimum speed. Multi-material plates rate each toolpath against its own material. Warp-prone materials get layer-time and enclosure advice instead of fan advice.
- **Environment aware**: bed temperature, heated chamber (only on printers with chamber control, as the slicer itself does), a passive chamber estimate for enclosed printers without control, exhaust fan, part and auxiliary fans, layer height, and lateral heat conduction between neighbouring lines.
- **Self-calibrating**: the cooling constant is solved from each preset's slow-down layer time and maximum fan, so third-party presets calibrate themselves.
- **Calibration tower**: Calibration menu → Thermal tower prints a cone that runs hot towards the tip. Measure the height where the surface turns rough, enter it in Calibrate mode, and the comfort window for that filament type is stored and used from then on. An illustrated guide explains how to read and measure the print.
- Ships with its own test target (`thermal_quality_tests`).

### Settings Advisor (new)
A magic-wand button next to Orient. Say what the print is for (functional, visual, fast draft) and it analyzes the plate in the background: bed contact, flat tops, overhang and unsupported areas, thin walls, measured from the mesh and from pre-sliced layer outlines. It then suggests walls, infill, shells, layer height, supports, brim, Arachne, monotonic top, seam and ironing settings, each with a short reason. Apply all or only the checked rows, as one undo step. Settings that differ per object become object overrides.

### Splash screen
Says ON STEROIDS, with a rotating quip.

### Slicing and G-code fixes
- Top surfaces: full-density rectilinear tops use monotonic ordering, removing the raised ridges that started at every hole.
- Supports: top and bottom Z gaps under half a layer were rounded to zero on thick layers (0.6/0.8 mm nozzles), printing supports straight against the part. At least one layer is kept.
- Tree supports: the contact filter offset was effectively zero, seeding tips against walls. The soluble flag and slice closing radius were frozen in statics across slices.
- Fast purge without a prime tower used the standard flush multiplier (about 17 % short).
- Scarf seams: forced scarf on compensated circles bypassed the first-layer and spiral guards and doubled the seam gap; the overhang check compared scaled lengths with millimetres so only the first path was checked.
- Spiral vase: smoothing rescaled E words with absolute extrusion, producing retract and blob jumps.
- Lazy z-hop after custom G-code travelled at layer height before lifting.
- Per-filament variant options (temperatures, retraction, max volumetric speed, z-hop, wipe, cooling minimum speed) read the wrong row on multi-nozzle machines; infill combining used the wrong nozzle diameter.
- A bare `M1020` line crashed the G-code processor.
- Data races in volume intersection and tree support generation.
- Int128 high word on Windows ARM64 used an unsigned multiply, which could flip orientation tests.

### Stability and UI fixes
- Send to printer: late plugin callbacks wrote into freed memory and GTK was touched from a worker thread, causing crashes and heap corruption.
- Top bar: hovering the expand buttons froze the whole app on Wayland (popup input grab); a native tooltip is used on Linux.
- Parameter tooltip cards stayed on screen under XWayland.
- Thermal Index colour schemes no longer reappear after a plain reslice.
- Build: OSMesa is linked only when present (Mesa 25.1 and later no longer ships it).

### Performance
Fewer copies per frame and per layer: option definitions, plate lists and the vendor database are no longer copied while painting; G-code generation copies region and object configs directly; layer lookups use binary search; triangle annotations build in linear time.

---

Bambu Studio is a cutting-edge, feature-rich slicing software.  
It contains project-based workflows, systematically optimized slicing algorithms, and an easy-to-use graphic interface, bringing users an incredibly smooth printing experience.

Prebuilt Windows, macOS 64-bit and Linux releases are available through the [github releases page](https://github.com/bambulab/BambuStudio/releases/).

Bambu Studio is based on [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, which is from [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community.

See the [wiki](https://github.com/bambulab/BambuStudio/wiki) and the [documentation directory](https://github.com/bambulab/BambuStudio/tree/master/doc) for more information.

# What are Bambu Studio's main features?
Key features are:
- Basic slicing features & GCode viewer
- Multiple plates management
- Remote control & monitoring
- Auto-arrange objects
- Auto-orient objects
- Hybrid/Tree/Normal support types, Customized support
- multi-material printing and rich painting tools
- multi-platform (Win/Mac/Linux) support
- Global/Object/Part level slicing parameters

Other major features are:
- Advanced cooling logic controlling fan speed and dynamic print speed
- Auto brim according to mechanical analysis
- Support arc path(G2/G3)
- Support STEP format
- Assembly & explosion view
- Flushing transition-filament into infill/object during filament change

# How to compile
Following platforms are currently supported to compile:
- Windows 64-bit, [Compile Guide](https://github.com/bambulab/BambuStudio/wiki/Windows-Compile-Guide)
- Mac 64-bit, [Compile Guide](https://github.com/bambulab/BambuStudio/wiki/Mac-Compile-Guide)
- Linux, [Compile Guide](https://github.com/bambulab/BambuStudio/wiki/Linux-Compile-Guide)
  - currently we only provide linux appimages on [github releases](https://github.com/bambulab/BambuStudio/releases) for Ubuntu/Fedora, and a [flathub version](https://flathub.org/apps/com.bambulab.BambuStudio) can be used for all the linux platforms

# Report issue
You can add an issue to the [github tracker](https://github.com/bambulab/BambuStudio/issues) if **it isn't already present.**

# License
Bambu Studio is licensed under the GNU Affero General Public License, version 3. Bambu Studio is based on PrusaSlicer by PrusaResearch.

PrusaSlicer is licensed under the GNU Affero General Public License, version 3. PrusaSlicer is owned by Prusa Research. PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

Slic3r is licensed under the GNU Affero General Public License, version 3. Slic3r was created by Alessandro Ranellucci with the help of many other contributors.

The GNU Affero General Public License, version 3 ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.

The bambu networking plugin is based on non-free libraries. It is optional to the Bambu Studio and provides extended networking functionalities for users.
By default, after installing Bambu Studio without the networking plugin, you can initiate printing through the SD card after slicing is completed.

