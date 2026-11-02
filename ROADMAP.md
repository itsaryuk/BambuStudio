# Roadmap

Planned features for BambuStudio ON STEROIDS, picked from the most-requested upstream issues that the slicer itself can solve (no firmware or cloud changes).

## 1. Spool inventory (built, not yet checked on a real print)
Upstream: [#2386](https://github.com/bambulab/BambuStudio/issues/2386), [#1186](https://github.com/bambulab/BambuStudio/issues/1186).

- Keep a local list of spools: filament preset, colour, net weight, empty-spool weight, grams left.
- Link a spool to an AMS slot or the external spool; AMS spools with an RFID tag match by tag, others are picked by hand.
- After each print, subtract the grams the G-code used per slot. A cancelled print subtracts the share printed so far.
- Before sending, warn when a slot's spool has less than the print needs.
- Correct a spool by weighing it (total weight minus empty spool).

Done when a print sent from the app lowers the right spools by the sliced grams, and a print that needs more than a spool holds is flagged in the send dialog.

## 2. Print queue and scheduled start
Upstream: [#2485](https://github.com/bambulab/BambuStudio/issues/2485), [#994](https://github.com/bambulab/BambuStudio/issues/994).

- Queue sliced plates per printer, with the send options (AMS mapping, bed levelling, timelapse) saved per job.
- Send the next job when the printer reports it is idle, and only after the user confirms the plate is cleared (a click in the app, or an opt-in auto-send for printers with a plate-clearing setup).
- Optionally start a job at a chosen time instead.
- The queue survives restarts; the app has to be running to send.

Done when two queued plates print one after the other with one confirmation in between, and a scheduled job starts at its time.

## 3. Duplicate plate (built, G-code comparison pending)
Upstream: [#1595](https://github.com/bambulab/BambuStudio/issues/1595).

- Plate menu → Duplicate: a new plate right after it with copies of every object (same positions, rotations, per-object settings and painted colours) and the plate's own settings (plate type, print sequence, filament order, name).

Done when the duplicate slices to the same G-code as the original apart from the plate number.
