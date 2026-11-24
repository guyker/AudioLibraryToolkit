# F-009: Menu option 5

Status: removed

## Behaviour
Option 5 has been removed from the legacy menu and its help. Selecting 5 is rejected as an invalid selection.
Options 6 and 7 retain their existing numbers.

## Entry points
- `app/AudioLibraryToolkitMain.cpp#RunLegacyMenu`

## Design and knowledge
- DESIGN: `AudioLibraryToolkit/DESIGN.md` → Entry points

## Acceptance checks
- [ ] Option 5 is not listed; selecting 5 is rejected without preparing an operation.

## Known limitations
- None.
