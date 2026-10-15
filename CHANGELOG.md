# Changelog

## 2026-10-15: the campaign's mission computer, and a camera shake

A player reported three bugs at the campaign hub. All three are fixed, and the player confirmed the fixes in the
headset (Meta Quest 3, Virtual Desktop).

### Clicking a mission did nothing, even with the real mouse

- **Cause.** With `[Performance] frameBMode=2` (the default), the right eye's frame runs no game update but still
  draws everything, including the game's in-world screens. The computer's script (`data/ui/terminal.lua`) reads
  the mouse buttons in its `draw()`, so it saw the left-eye frame's click a second time. A mission row is a
  toggle: the second click unselected it at once, and its PLAY panel never opened.
- **Fix.** The screen the player uses is drawn in the left-eye frame only (`Hook_ScreenRender` on
  `Screen::render`). Its texture, cursor included, is the same for both eyes. The other screens are drawn as
  before.
- **Setting.** `[Controles] screenOncePerPair` (default 1).

### The controllers could not move the computer's cursor

- **Cause.** A screen the player sits at moves its cursor by the raw mouse motion only. The controllers make
  none.
- **Fix.** In every left-eye frame, the pointing hand's ray is intersected with the screen's mesh. The hit's UV
  gives the pixel, as in the engine's own touch hit test. That pixel replaces the one in the cursor event that
  `Screen::update` sends (`ScreenPointerUpdate`, `Hook_ScreenCursor`).
  - The dominant hand points when the player sits down.
  - The other hand takes the pointer by pulling its trigger with its ray on the screen. That pull is already a
    click, where it points.
  - The pointing hand's trigger is the left mouse button. At the computer, the left trigger no longer crouches,
    and a pull begun with the ray beside the screen does not click.
  - The real mouse still works: moving it takes the cursor, and the laser takes it back with a click or a quick
    motion (24 pixels in a quarter of a second).
- **Setting.** `[Controles] screenPointer` (default 1): "In-game computer with the controllers" in the settings
  panel.

### The view shook when a wrist turned, walking and seated at the computer

- **Cause.** The eye position is the game's camera position plus the head offset that the Lua mod sets with
  `SetCameraOffsetTransform`. The game expresses that offset in its camera's rotation. The mod expressed it in the
  new aim of the current frame, but the game applies that aim (player yaw `+0x704`, pitch `+0x700`) only at the
  next `Player::update`, so the camera rendered this frame still has the previous aim. Every aim change swung the
  eye by the aim's angle times the head offset. Seated at a screen, the game also glided its camera in front of the
  screen, away from the hands and lasers.
- **Fix.**
  - `main.lua` builds the offset in the rotation the game actually renders (`pc0.rot`). The new aim is used only
    when `SetPlayerTransformWithPitch` turned the camera at once (roomscale steps).
  - While the player uses a screen, the Lua mod tags it `nocam`: the game's own tag for a screen without a camera
    glide. The DLL also keeps the game's glide value at 0, because `nocam` only freezes it where it is.
- **Verification.** On the mock headset, aim steps of 20 to 30 degrees with a 0.3 m head offset moved the eye by
  135 mm per step before the fix, and by 0.0 mm after it.

Details: [docs/PROBLEMS_AND_SOLUTIONS.md](docs/PROBLEMS_AND_SOLUTIONS.md) (C9, K4 to K6) and
[docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md) (section 7 quinquies).
