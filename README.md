![image](https://user-images.githubusercontent.com/106916061/179006347-497d24c0-9bd6-45b7-8c49-d5cc8ecfe5d7.png)
# BambuStudio ON STEROIDS

A fork of Bambu Studio (based on the 2.8.3 beta) with a built-in thermal simulation, a settings advisor, and a batch of slicing, G-code and stability fixes. Everything runs offline on your computer. No account, no cloud, no subscription.

## What is different from the original

### Thermal Optimization (new)
A thermometer button in the top bar opens one dialog with Analyze, Optimize and Calibrate. Analyze and Optimize unlock once the plate is sliced.
- **Analyze** simulates how warm each layer still is when the next one lands and rates every toolpath from cold (-1) to hot (+1). The result shows up as the Thermal Index colour schemes in the preview, with an outcome, the direction of the problem (overcooling or overheating), per-layer caveats and concrete suggestions.
- **Optimize** rescales layer speeds so every layer lands just under the hot edge of the material's comfort window, within your limits (speed-up, slow-down, volumetric cap, keep outer walls). Cool layers speed up to the flow cap (up to 3x by default) and are never slowed; hot layers may go below the filament's minimum speed, down to the slow-down limit (50 % by default). Layer 0 and quality-sensitive roles are left alone. M73 progress lines are recomputed, so the printer's time estimate stays right.
- **Cooling pauses and cooling tower** for layers that stay hot even at the slowest speed: Optimize can park the head clear of the part for the seconds the model asks for (capped per layer, 15 s by default), and Analyze can add a hollow cooling tower beside the parts so small layers gain time without pausing.
- **Material aware**: one model per filament slot from the filament presets (PLA, PETG, ABS, ASA, PC, PA, PAHT, PPA, PPS, PP, PE, TPU, PVA, PEEK, PEI and their CF/GF variants), with the preset's vitrification temperature, maximum volumetric speed and slow-down minimum speed. Multi-material plates rate each toolpath against its own material. Warp-prone materials get layer-time and enclosure advice instead of fan advice.
- **Environment aware**: bed temperature, heated chamber (only on printers with chamber control, as the slicer itself does), a passive chamber estimate for enclosed printers without control, exhaust fan, part and auxiliary fans, layer height, and lateral heat conduction between neighbouring lines.
- **Self-calibrating**: the cooling constant is solved from each preset's slow-down layer time and maximum fan, so third-party presets calibrate themselves.
- **Calibration tower**: Calibrate walks through three steps: print a cone that runs hot towards the tip, read it with the illustrated guide, enter the height where the surface turns rough. The result is stored per filament type and printer model (printers differ in airflow), and the dialog warns when a calibration came from another printer or a very different layer height.
- Ships with its own test target (`thermal_quality_tests`).

### Settings Advisor (new)
A magic-wand button next to Orient. Say what the print is for (functional, visual, fast draft) and it analyzes the plate in the background: bed contact, flat tops, overhang and unsupported areas, thin walls, measured from the mesh and from pre-sliced layer outlines. It then suggests walls, infill, shells, layer height, supports, brim, Arachne, monotonic top, seam and ironing settings, each with a short reason. Apply all or only the checked rows, as one undo step. Settings that differ per object become object overrides.

### Arrange: dense packing (new)
"Arrange objects on current plate" fits far more onto the plate. It packs each part's real footprint instead of its convex hull, so parts nest into each other's corners and small parts go inside openings. With "Auto rotate for arrangement" (now on by default) parts turn 90°, 180° or 270° or to their tightest angle. It tries a dozen packing orders and keeps the one that fits the most; when everything fits the pile stays close to square and centred, clear of the prime tower and excluded areas. In tests on a 256 mm bed: 16 → 24 L-shapes, 15 → 27 triangles, 27 → 36 T-shapes. By-object printing and "Arrange all" keep the old engine, which now turns in quarter turns instead of 45° diagonals. Test target: `arrange_dense_tests`.

### Purge reduction (new)
- The filament order of all layers is planned together (minimum total flush over the whole print, not one layer at a time).
- Flush into infill turns on automatically where it cannot show: enough walls and shells, same material, no translucent filament, flushed colour not much darker than the walls (option "Automatic flush into infill").
- Measured purge data can lower the colour-based flush estimate, not only raise it.
- After a big purge the prime tower primes less (up to 15 mm³).
- Measured on real plates: 1.4–2 g and about 2.5 minutes less on a small three-colour plate, 9–10 % less filament on five-colour towers. Test target: `purge_order_tests`.

### Preview: section view (new)
Tick "Section view" at the bottom of the legend to cut the toolpaths with a movable plane (rotation, tilt, offset) and look inside the print.

### Duplicate plate (new)
Plate menu → Duplicate Plate adds a copy right after the plate: every object on it (positions, rotations, per-object settings, painted colours) and the plate's own settings (plate type, print sequence, filament order, name).

### Spool inventory (new)
The Filament Manager keeps its spools on your computer (`filament_inventory/spools.json`), with or without a Bambu account. When a print sent from the app finishes, the grams it used come off the spool in each AMS slot; a cancelled or failed print takes the share printed so far. Spools with a Bambu RFID tag are left to the printer's own reading. The send dialog warns when an untagged spool holds less than the print needs. Prints started from the printer screen or another app are not counted.

### Presets and printing
- Copy a user filament or process preset to other printers: your changes are laid over the matching system preset of each chosen printer, so the copy keeps that printer's nozzle and speed values.
- Send print job: hovering a filament shows the flow dynamics profile and K value the printer holds for that slot, and a line explains what Auto, On and Off calibration do with the selected profiles.

### Camera view
- Dragging moves the zoomed live view (the original zooms with the wheel but could only zoom on the centre).
- Each video frame has its own memory (frames could be overwritten while drawn), it is converted for drawing once per frame, and repaints are no longer forced synchronously.

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

