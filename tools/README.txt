===============================================================================
                         TEARDOWN VR  -  mod for Quest 3
===============================================================================

Play Teardown in virtual reality, like a native VR game:
  - true 3D: both eyes show exactly the same moment of the game
  - aim and hold your tool in your hand, with an aiming laser
  - stick movement, snap or smooth turning, roomscale
  - settings panel in the game (and in the headset)
  - menus, loading screens and pause shown on a big virtual screen

-------------------------------------------------------------------------------
1. WHAT YOU NEED
-------------------------------------------------------------------------------
  - Teardown (Steam version 2.0.1)
  - A Quest 3 connected to the PC with Virtual Desktop (recommended), Meta
    Horizon Link or SteamVR. With Virtual Desktop, keep the "VDXR" runtime (the
    default setting): it's the fastest. When the game starts, the mod finds out
    on its own where the headset is (Virtual Desktop or Quest Link): nothing to
    set up in Windows.
  - A PC that can run Teardown at about twice the headset's refresh rate (see
    "Performance").

-------------------------------------------------------------------------------
2. INSTALLATION
-------------------------------------------------------------------------------
  1. Close Teardown.
  2. Double-click "Installer Teardown VR.exe" (it finds the game on its own
     through Steam), choose your language ("English" or "Français"), then click
     "Install Teardown VR".
     If Windows shows "Windows protected your PC": click "More info", then
     "Run anyway" (Windows shows this for small programs it doesn't know yet).
     To update: same thing, with the new version (your settings are kept).
  3. Start Teardown, then: Play > Mod manager > "Built-in" list >
     "Teardown VR" > click "Disabled" to enable it.
     (Only needed once.)
  4. The game's display must be set to "Borderless window" or "Window": in
     "Fullscreen", the game renders at the PC screen's resolution (1920x1080
     for example) and the image is blurry in the headset. The mod switches it
     back to "Borderless window" on its own every time the game starts.

  Language: the mod (settings panel, messages in the headset) is in the
  language chosen in the installer, English by default. You can change it at
  any time in the settings panel: Display tab, first line "Language / Langue".
  The panel switches at once.

  Starting settings: a first install starts with the recommended settings for a
  Quest 3 (image quality "Headset native (Quest 3)", distance sharpness
  "Maximum", sharpening 100%, default headset refresh rate, same particles in
  both eyes...), tuned for a very powerful graphics card. On a less
  powerful PC, if it stutters, lower "Image quality" ("Very high" or "High") in
  the settings.
  An update keeps your own settings (TeardownVR.ini, in the game folder). The
  "Reset all settings" button brings back the starting ones.

  Note: "Distance sharpness" (except "Game's own") and "Same particles in both
  eyes" use shaders rebuilt from the game's own ones, which are not part of
  the public source code. In a version built from the public source code, the
  game keeps its own anti-aliasing and particles, and these two settings
  change nothing.

-------------------------------------------------------------------------------
3. PLAYING
-------------------------------------------------------------------------------
  1. Put on the headset and connect it to the PC: Virtual Desktop, or Quest
     Link (cable or Air Link).
  2. Start Teardown (from the Virtual Desktop desktop or from the PC).
  3. Menus are shown on a virtual screen: aim with a controller, trigger =
     click, B = back. The hand that clicked last keeps the pointer (orange
     square); the other hand (small gray square) takes it when you press its
     trigger while aiming at the screen.
  4. Start a level: you switch to immersive VR.

  If you're not standing in the right place: hold the left stick click (or F8)
  to recenter the view.

-------------------------------------------------------------------------------
4. CONTROLS (right hand = dominant hand, swapped in left-handed mode)
-------------------------------------------------------------------------------
  Right trigger .............. use the tool (with the sledge: a short, light
                               press is enough for a full swing; you can also
                               hit things directly with its head)
  Right grip ................. grab / carry an object
  Right stick left/right ..... turn (smooth by default; snap turning in
                               Settings > Comfort)
  Right stick up/down ........ switch tool (in the AC130 plane view: zoom
                               in / zoom out). While holding an object (right
                               or left grip): push it away (up) or bring it
                               closer (down)
  Right stick click .......... show / hide the inventory: the game's own
                               (weapons and tools, mods included, sorted by
                               group), a window floating in front of your belt.
                               Turning your head doesn't move it; it follows
                               you when you walk and when you really turn
                               around (your whole body). Look at it and aim at
                               a name + trigger to take the tool (a group
                               number: the group's next tool). Long list: hold
                               the trigger on it and move your hand up or down
                               to scroll it (like on a phone).
                               To move it: aim at it and squeeze that hand's
                               grip (the button that grabs objects), move your
                               hand, let go: it stays there, even the next time
                               (yellow frame and a small vibration while you
                               hold it). It stays shown until the next click.
                               If it was left far from your gaze (behind you),
                               the click brings it back to its place around you
                               (straight ahead, or on the side where you put
                               it) instead of hiding it.
  A .......................... jump
  B .......................... flashlight
  Left stick ................. move
  Left trigger (L2) .......... crouch (while held); with the Total Overdose
                               weapon: fire the left pistol (see "Dual wield")
  Y .......................... interact (E): get in / out of a vehicle
  In a vehicle ............... right trigger = accelerate, left trigger = brake
                               then reverse, left stick = steer (halfway =
                               steer halfway), A = handbrake, Y = get out,
                               grips = the machine's arm (up / down). The view
                               turns with the vehicle.
  Left grip .................. grab with the left hand, like the right grip:
                               from afar, an object aimed at with the left
                               laser (within 3 m, the hand icon appears on it)
                               is held at its distance and follows your hand;
                               up close, an object under your hand is taken in
                               your hand. Release = drop / throw (see
                               "Physical hand"). With the Total Overdose
                               weapon: crouch
  Left hand .................. your left hand and its forearm are drawn in the
                               game and follow the controller. It's a real
                               physical hand (like Bonelabs): it stops against
                               walls and pushes what moves. It also aims at
                               menus (settings, inventory, game menus, pause):
                               the hand that clicked last keeps the pointer;
                               the other one takes it when you press its
                               trigger while aiming at the menu (that click
                               already counts).
                               It has its own laser, like the right hand: the
                               laser stops on menus (settings, inventory, a
                               mod's menu) to show what you're aiming at.
  X .......................... 3D map of the level (a scale model seen from
                               above: left stick = move over it, right stick
                               up/down = zoom, left/right = rotate around it,
                               X = close)
  Menu (short press) ......... pause
  Menu (long press) .......... Teardown VR settings panel
  Left stick click (held) .... recenter the view
  Right stick click (HELD) ... KEYS panel: every key of the keyboard and mouse,
                               like on PC (see below)
  Left stick click (short) ... quick key: the last key used in the keys panel
                               (e.g. Q)

  On the keyboard: F10 = settings panel, F8 = recenter.

  KEYS PANEL (hold the right stick click for half a second; in a vehicle a
  short click is enough; aim with the right controller)
    - "Mod keys": the keys of the active mods, found on their own in their
      files (even for a mod installed tomorrow). E.g. with AC130 Airstrike: Q =
      previous shell, O = options, E = next shell (already on Y).
    - "Keyboard": a full keyboard (QWERTY). Shift, Ctrl and Alt stay held until
      the next key press.
    - "Mouse": left / right / middle click, wheel up / down, and the stick that
      acts as a mouse.
    - Trigger on a key: short press = the key is pressed; trigger held = the
      key stays held down; trigger held while sliding up or down = the list
      scrolls (like on a phone).
    - This panel and the settings panel are windows that float where they open,
      in front of you: turning your head doesn't move them. To move one: aim at
      it and squeeze that hand's grip, move your hand, let go (orange frame
      while you hold it). It will then open at that distance and height, in
      front of your gaze (Settings > Screen: "Default distance and height" to
      go back to the starting place).

  MOD CAMERA (AC130 plane, drone, turret...)
    When a mod takes the camera, the left stick acts as a mouse to aim (your
    character stops walking meanwhile). The right stick keeps turning, and
    up / down = mouse wheel: that's the AC130's zoom (up = zoom in, down = zoom
    out; stick held = continuous zoom). Zooming moves the camera closer to the
    point you aim at, like a drone flying forward: the target gets bigger (up
    to x16) with no distortion at all, you look around normally, depth stays
    natural and the crosshair stays on the target. The camera always stops
    before the ground or a wall. When you leave the plane view, the view goes
    back to normal (the AC130 keeps its zoom level for next time). Adjustable
    in the keys panel, "Mouse" tab (other stick, speed, "Always" or "Never");
    the zoom type ("Move closer" or "Magnifier", the old mode) is chosen, and
    the zoom turned off, in the settings (Display, "Mod camera zoom").

  IN-GAME COMPUTER (the campaign's mission computer, at the hub)
    Y to sit at it. The view stays where your head is, with your hands and your
    lasers (it no longer glides in front of the screen).
    - The laser of the hand holding the tool moves the computer's cursor; its
      trigger clicks (choose a place, then a mission).
    - To point with the other hand: aim at the screen with it and pull its
      trigger (L2 or R2). That click already counts, where it aims. The other
      hand takes the pointer back the same way.
    - A trigger pulled while its laser is beside the screen does not click.
    - Meanwhile the left trigger no longer makes you crouch.
    - The real mouse still works: move it and it takes the cursor; aim and
      click with a controller to take the cursor back.
    - Y leaves the computer. Settings (Controls): "In-game computer with the
      controllers" (off: as before).

  MOD MENUS WITH THE MOUSE (THE EVERYTHING REVOLVER, Natural Phenomena's
  weather, item spawn menu...)
    When a mod opens a window you use with the mouse, it's shown sharp in front
    of you.
    - A menu (Natural Phenomena's weather: "Natural Phenomena" button in the
      pause menu; item menus...) opens as a FLOATING WINDOW placed in the room,
      straight ahead of your gaze, 1 m away. It no longer moves when you turn
      your head: you can look at it from the side, or look at the sky above it.
      If you really turn around (your gaze stays more than 10° away from the
      window - and from the direction you were looking when it was placed - for
      half a second) or if you walk away from it, it gently follows you and
      takes back its place relative to your gaze (straight ahead if you haven't
      moved it) - never while you're aiming at it or moving it.
    - To move it: squeeze the grip (the same button you grab objects with)
      while aiming at it, move your hand, let go. It stays where you let it go,
      upright and facing you, and that menu will reopen at the same place
      relative to your gaze. Put to the side, it stays there as long as you
      look ahead, toward it or in between. If the window uses right click (or
      if there's no way to know whether it does), the grip of the hand aiming
      at it stays its right click: then aim just outside its edge (the dot
      turns blue and its corners are marked in blue), or squeeze your other
      hand's grip while aiming at it. If your free hand touches an object, its
      grip grabs the object, not the window. Under the settings panel or the VR
      inventory (always shown on top), the grip takes that panel.
    - When no controller is aiming at the floating window, the dot disappears
      and the mouse is "outside" it: pulling the trigger while aiming
      elsewhere = clicking outside the window (Natural Phenomena closes, as on
      the PC screen); no button is clicked by mistake, even if you keep the
      trigger pressed while bringing your aim back onto the window.
    - If another mod also opens a mouse window at the same time, both stay in
      front of your head, as before (one mouse for both), as long as the other
      one is open. A blurred background (UiBackgroundBlur) isn't drawn on a
      floating window.
    - The revolver's bullet menu (X opens / closes it) opens straight ahead of
      you, at eye level, as if you were looking straight ahead - even if you're
      looking down at your weapon at that moment. The ring is drawn as on the
      PC screen: it no longer follows your hand or wrist. It stays in place in
      the room when you turn or lower your head (its side panels too) and only
      follows you when you move (with the stick, or on foot as soon as you take
      a step). The grip doesn't move it: it stays the revolver's right click.
      To bring it back in front of you: close and reopen the menu (X). Settings
      (Display): "Revolver wheel in front of me" and
      "Revolver wheel fixed in the room" (off: in the center of your view, as
      before).
    Aim at the window with a controller, left OR right: an orange dot follows
    the aim of the hand that clicked last. That hand's trigger = left click;
    hold it to drag (revolver: take a bullet on the wheel and drop it on a
    chamber of the cylinder). That hand's grip = right click (revolver: empty a
    chamber). To switch to the other hand, aim at the window with it and pull
    its trigger (just aiming isn't enough). While the window is open, the left
    trigger no longer makes you crouch. Settings (Display):
    "Mod menus as floating windows" (off: the window stays in front of your
    head, as before), "Mod menu size", "Forget mod menu positions",
    "Mod menu distance".

  PHYSICAL HAND (left hand, or right in left-handed mode; like Bonelabs)
    - Your free hand is a real game object, as strong as the hand holding the
      sledge (same strength, same weight). It goes through nothing: against a
      wall or a table, it stays at the surface (your drawn hand stops there,
      the forearm follows) and slides along it. It pushes and drags what moves:
      a crate slides away, a very heavy object resists. The controller only
      vibrates on real contact (impact, pressure).
    - It never pushes you and can't get you stuck. It lets through shots, the
      tool you're holding and what the game grabs: it turns "ghost" when it's
      in front of your weapon or in your line of aim while you shoot, press Y
      (interact) or the right grip (the game's grab), and near the object the
      game is carrying. Elsewhere it stays solid: it can hold a door back while
      you shoot with the other hand.
    - Grab: put your hand on an object that moves (or within 8 cm of it) and
      squeeze the left grip: it's in your hand. It follows your hand with an
      arm's strength: a light object follows quickly, a heavy one slowly, and
      one too heavy (or stuck) slips out of your hand. A door swings on its
      hinges. Release the grip to drop it; make a throwing motion and release:
      it flies off with your hand's speed. While you hold it, you pass through
      it (it doesn't push you); once dropped, it becomes solid again for you
      and your hand as soon as it no longer touches you.
    - Grab from afar (like the right hand): aim at an object with the left
      hand's laser. If it can be taken (within 3 m, 10 tons at most), it gets a
      white outline, the laser lights up and the game's hand icon appears where
      you aim. Squeeze the left grip: you hold it where you picked it up, at
      its distance (even very close), at the end of the laser; right stick
      up / down: farther / closer; it follows your hand (turn your wrist: it
      turns around it) and passes through you without pushing you. A heavy
      object follows more slowly; blocked far from the end of the laser, it's
      dropped. Release the grip to drop it; make a throwing motion and release:
      it flies off with its speed.
    - Left grip = grab, always (like the right grip). Nothing under the hand or
      at the end of the laser: nothing happens; release, then squeeze the grip
      again near the object. Crouch: left trigger (L2), even while holding an
      object. To pick something up from the floor, aim at it with the laser,
      crouch (left trigger) or bend down for real.
    - The right grip always grabs like the game does (with the laser). If it
      takes the object your left hand is holding, the left hand lets go.
    - Settings > Controls: "Physical hand (like Bonelabs)" and
      "... grab with the grip" (off: the hand goes through everything, the left
      grip makes you crouch like the left trigger); "Left hand laser" (off: no
      left laser, and no grabbing from afar).

  DUAL WIELD (the "Total Overdose" mod weapon, two pistols)
    When you take Total Overdose, you have a pistol in EACH hand (instead of
    both in the right hand):
      Right trigger (R2) ........ fires the right hand's pistol
      Left trigger (L2) ......... fires the left hand's pistol
      Left grip ................. crouch (while squeezed): your left hand holds
                                  a weapon, it doesn't grab anything
      Right grip ................ nothing (on PC, right click does the mod's
                                  dodge, a roll on the ground seen from
                                  outside, unpleasant in VR: it's disabled)
    Each bullet leaves from its pistol's barrel, toward the point aimed at by
    that hand's laser (the left laser is always there with this weapon; if you
    turned it off, a small cross shows the aimed point). One press = one shot,
    like two real guns; press fast on both sides and the shots follow one
    another. The controller that fires vibrates, and each pistol has its own
    recoil.
    - If your left hand is already holding an object when you take the weapon
      (or if its controller is no longer tracked), the left pistol is hidden
      and the left trigger does nothing until you let go of the object.
    - While the right grip is carrying an object, the left trigger doesn't fire
      (the game would throw the object).
    - In left-handed mode, the two grips are swapped: right grip = crouch, left
      grip = nothing (and what is said here about the left hand applies to the
      right one). Each trigger still fires its own hand's pistol.
    - Settings > Controls:
      "Dual wield: one weapon in each hand (Total Overdose)" (on). Off: both
      pistols in the right hand, fired one after the other by the right
      trigger, left trigger = crouch, as before.

-------------------------------------------------------------------------------
5. SETTINGS (long-press Menu, or F10)
-------------------------------------------------------------------------------
  Everything is saved automatically in TeardownVR.ini (in the game folder).

  Display
    - Language / Langue (first line): "English" or "Français". The panel
      switches at once.
    - Image quality (resolution): from "Low" to "Headset native (Quest 3)"
      (3024 x 3264 per eye, the resolution recommended by Virtual Desktop;
      starting setting).
      The higher it is, the sharper the distance. The game reads this setting
      at startup: restart Teardown to apply it.
    - Field of view covered: 100% = the whole view. Less = the game only draws
      the center of the view: the edges turn black, and what remains keeps its
      size and shape (no zoom). The game renders a correspondingly smaller
      image, just as sharp: smoother. The black border changes right away; the
      performance gain comes the next time the game starts.
    - Resolution sent to the headset, Headset refresh rate.
    - Stereo: "Synchronized" (recommended, true 3D) or "Alternating eyes"
      (fallback, if a game update breaks the synchronized mode).
    - Sharp image per eye (recommended): each eye keeps its own anti-aliasing,
      otherwise the image is blurry.
    - Exact field of view per eye: each eye renders exactly what the headset
      shows (wider outward than toward the nose), with no wasted pixels.
    - Distance sharpness: replaces the game's anti-aliasing, which makes the
      distance blurry in a headset (your head always moves a little). "Maximum"
      (starting setting): the sharpest; if distant edges flicker too much for
      your taste, pick "Natural" (distant detail kept, without an oversharpened
      look or a checkerboard pattern on glass) or "Very sharp" (a bit more
      detail, slight halo on edges). "Sharp (smooth)" smooths more. "Game's own"
      to compare. Takes effect immediately.
    - Remove distant haze: removes the level's haze during VR (the distance is
      sharper and less washed out). Off by default: the haze of the level and
      of the weather (rain, snow) is part of the atmosphere.
    - Remove light glow (bloom): the game spreads the sky's light into a glow
      that veils distant objects like a blur. On by default.
    - Remove depth blur: never any "movie camera" blur.
    - Same particles in both eyes (rain, smoke): rain, smoke (extinguisher,
      explosions), fire and dust are drawn as small, tightly packed dots that
      the image then smooths. Before, each eye saw different dots on the same
      smoke, which the brain can't fuse (flicker, eye strain). On (the
      default): both eyes see the same dots, at the same spot on each particle,
      lit from the same side, with the same light grain (soft shadow, sun); the
      left eye doesn't change. Takes effect immediately.
    - Moving smoke: same smoothing in both eyes: the game smooths smoke from
      frame to frame by following each particle's motion. Before, the right eye
      never saw that motion (its smoke smeared and was smoothed differently).
      On (the default): the right eye follows the smoke's real motion, like the
      left eye. Takes effect immediately.
    - Smoothness:
        "Mod objects in the same place in both eyes": some mods move their
        objects forward by a fixed step at each update (the AC130 plane:
        15 cm). The right eye saw it farther ahead than the left eye, as if
        doubled. On: both eyes see each object in the same place.
        "Particles synced between eyes": smoke, sparks and debris are computed
        at the same time as the rest of the game; both eyes see them in the
        same place (before: up to 10 cm apart during explosions).
        "No catch-up after a stutter": after a late frame, the game no longer
        runs two physics steps at once (which caused chains of stutters during
        big explosions); it slows down imperceptibly instead.
    - Sharpening (filter): 0 = the image as the game renders it, 1 = the
      sharpest (starting setting).
    - Sharpening, Brightness, Eye separation, World scale, HUD, Aiming laser.
    - Mod camera zoom (on by default): when a mod zooms by narrowing its field
      of view (AC130 plane view: right stick up / down), what you aim at gets
      bigger in the headset, up to x16. A field of view of 60° or more stays at
      real size. Off: a mod's view is never enlarged.
      Zoom type: "Move closer" (recommended) moves the camera toward the point
      you aim at, with no distortion at all. "Magnifier" (old mode) enlarges
      the image: its edges warp when you turn your head (fisheye effect) and
      depth looks flat.
    - Mod crosshairs on the controller's aim: the crosshair a mod weapon draws
      in the center of the screen (portal gun logo, chaingun crosshair...) is
      placed on the point the laser aims at, at its distance, instead of
      staying in front of your eyes. The rest of the HUD doesn't move. (Setting
      shown when "HUD in the center of view" is on.)
    - Mod menus with the controller (on), "Mod menus as floating windows" (on),
      their size and their distance (1 m): see "Mod menus with the mouse"
      above.
  Comfort
    - Turning: "Snap" (angle) or "Smooth" (speed); movement direction and
      speed, roomscale, real height or the character's height, height offset,
      anti-nausea vignette, "Recenter view (F8)" button.
  Controls
    - Free hand's arm (drawn or not), with "Physical hand (like Bonelabs)" and
      "... grab with the grip" (see "Physical hand" above); both hands point at
      menus (otherwise only the hand holding the tool); left hand laser (on).
    - Left-handed mode, aim with the controller or the head, vibration, tool
      position and tilt in the hand.
    - Dual wield (on): with Total Overdose, a pistol in each hand (see
      "Dual wield" above).
    - Inventory: height, distance and side (or move it directly with the grip,
      see "Right stick click"), "It follows me when I turn around" (on: when
      you really turn around, turned more than 55° for more than a second, it
      comes back in front of you; a glance to the side doesn't move it),
      "Put the inventory back at my belt" button.
    - Physical sledge: the sledge's head breaks what it touches, in every
      direction, with the strength of the game's sledge (wood, plaster,
      glass... not metal or concrete). The faster the swing, the bigger the
      hole. The trigger still does the game's normal swing.
    - Sledge: a light press = a swing (on): with the sledge in hand, a light,
      short press on the trigger does a full game swing, which breaks what the
      laser aims at up to 3 m as in the base game; no need to keep the trigger
      held down. Off: you have to press firmly, as with weapons.
    - Tools collide with objects + Physical tool (like Bonelabs): the tool in
      hand (sledge, weapons, extinguisher...) is a real physical object, held
      by your hand with an arm's strength. It goes through nothing: it stops
      against walls, slides along surfaces, pushes, drags or lifts what moves
      (a heavy object resists and the tool shifts in your hand). The controller
      only vibrates on real contact (impact, pressure); the tool follows your
      hand without lagging or shaking, even when the game slows down. If it
      stays stuck far from your hand, it comes back to it. While you shoot
      (trigger) or carry an object, it doesn't block the shots or the carried
      object.
      Without "Physical tool": the old mode (the tool goes through things while
      pushing them, or stops at the surface).
    - Mod weapon size: mods draw their weapons for a PC screen, far in front of
      the camera; held in your hand they looked huge (a 1.5 m revolver). Each
      mod weapon, even one installed later, is automatically shrunk to the size
      it has on screen (1.1 m long at most). 100% = that automatic size; set it
      smaller or bigger to your taste. Shots, flashes and beams come out of the
      shrunk weapon's barrel, and its movements (recoil...) are at its size.
  Screen
    - Distance and size of the virtual screen, cinema mode (always on the
      screen, no 3D), size of the settings panel in VR, distance and height
      where it opens (remembered when you move it with the grip),
      "Default distance and height" button.
    - "16:9 pause menu (like on the PC screen)" (on by default): when paused in
      a level, the virtual screen has the shape of a PC screen and the pause
      menu is laid out as on the PC (before: an almost square screen). Off: as
      before.
  Info
    - Headset and mod status, frames per second, reset.
    - Headset used: "Automatic (detection)" by default. When the game starts,
      the mod takes the one that has the headset: Virtual Desktop or Meta Quest
      Link (both connected: the one set in Windows). The "Detection" line says
      which one. A headset connected after the game starts is found too. You
      can also force Virtual Desktop, Meta Quest Link, SteamVR or
      "The one set in Windows" (the next time the game starts).

-------------------------------------------------------------------------------
6. PERFORMANCE
-------------------------------------------------------------------------------
  For both eyes to see the same moment, the game renders the left eye then the
  right eye one after the other (game time is frozen for the 2nd eye). So it
  takes about 2 game frames per headset frame:
      72 Hz in the headset  ->  aim for ~144 FPS in the game
  If it stutters:
    - choose 72 Hz in "Headset refresh rate"
    - lower Teardown's graphics options (shadows, reflections...)
    - lower "Image quality" or "Resolution sent to the headset"
  The headset (Virtual Desktop / Link) fills in missing frames, but it's nicer
  when the game keeps up.

  What the mod does on its own for smoothness (nothing to set, the image in the
  headset doesn't change):
    - during VR, the game runs at "above normal" priority in Windows (never
      higher: Virtual Desktop keeps priority); it goes back to normal when VR
      stops;
    - the TeardownVR.log file is written in the background, without slowing the
      game down;
    - the mod avoids doing some computations twice (mod weapons, moving
      objects, the mod's Lua script);
    - "CPU optimization" (Display tab, on by default): the right eye is only
      drawn (the game doesn't redo all its work a 2nd time), the CPU prepares
      the right eye while the graphics card is still drawing the left one
      (before, each waited for the other), and the next frame starts its work
      (scripts, physics) without waiting for the graphics card to finish the
      previous one.
      Measured: ~40% less time per frame when things are calm, and during big
      explosions ~35-45% fewer missed frames and ~50% fewer stutters. Same
      image. Turn it off to compare with the old behavior.
    - with "Remove light glow" and "Remove depth blur" on, the game no longer
      computes these two effects for nothing (before, it computed them and then
      multiplied them by zero): ~0.6 ms of graphics card time saved per headset
      frame, pixel-identical image (checked every 5 seconds; at the slightest
      difference, it turns itself off).
  What remains during the biggest explosions: the game engine computes each
  pile of debris as a single block on a single CPU core, and the graphics card
  then draws each eye in ~6 ms. Splitting that up would change the game's
  physics: the mod doesn't do it.
  Tips:
    - turn off the Steam overlay for Teardown (Steam > Teardown > Properties >
      uncheck "Enable the Steam Overlay while in-game") and, with an NVIDIA
      card, the NVIDIA overlay and "Instant Replay" (NVIDIA app, Alt+Z). If
      they're loaded in the game, the bottom of the settings panel says so in
      orange ("... overlay active").
    - close Chrome and other heavy programs while you play (Chrome was taking
      almost a whole CPU core during VR).
    - in TeardownVR.ini, leave debugLua=0: debugLua=1 is only for diagnosis
      (measurements in the log) and costs a little computing. Likewise,
      perfTrace=1 or 2 ([Debug] section) writes detailed measurements: only set
      it to measure performance yourself.

  Sharper image with Virtual Desktop (in the VD app on the headset, Streaming
  tab):
    - "VR Graphics Quality": Ultra or Godlike
    - "Video codec": AV1 10-bit (RTX 40/50 cards), around 200 Mbps.
      Tested with a top-end graphics card + Quest 3: AV1 10-bit at 200 Mbps
      is more stable than H.264+ at 500 Mbps, which stuttered. AV1 compresses
      about twice as well: 200 in AV1 is worth roughly 400 in H.264, with much
      less data going through the Wi-Fi. If it still stutters, lower the
      bitrate (150) rather than raising it.
      Why H.264+ at 500 Mbps stutters: each frame then weighs ~5.6 Mbit, and
      during explosions or while a level loads these frames no longer get
      through the Wi-Fi in time (or the headset takes too long to decode them);
      Virtual Desktop then holds the game back. The TeardownVR.log file counts
      these holds ("runtime holds"). The game can't stop the Wi-Fi from
      blocking, but since October 12, 2026 it resumes exactly where it stopped
      ("No jump after a streaming hitch" box, on by default): no more objects
      jumping forward or physics computed twice after each hitch. To get rid of
      the hitches themselves in H.264+: turn on "Video buffering" in the VD app
      (the headset keeps one frame ahead, ~11 ms of extra latency), or use a
      lower bitrate.
    - "Sharpening": to your taste (VD adds its own sharpening)
  The "slight compression" you can see comes from wireless streaming: more
  bitrate = less compression, but a bitrate too high for the Wi-Fi causes
  stutters (Virtual Desktop then holds frames back).

  Another program using the graphics card (AI computation, another game,
  rendering software...) makes the game drop to 15-20 FPS. The mod detects it:
  an orange banner shows in the headset for 10 seconds with the program's name,
  and the Info tab lists it. Close it or pause it (that was the cause of the
  20 FPS on October 10, 2026).

  Big stutters at times (smoke, destruction): see "Smoothness" in the Display
  tab (on by default). The rest comes from the graphics card running flat out:
  Virtual Desktop then puts the game on hold. Keep some headroom: a lower image
  quality ("Very high" rather than "Ultra"), and turn off the Steam overlay for
  Teardown (Steam > Teardown > Properties > uncheck
  "Enable the Steam Overlay while in-game"), which adds waiting to every frame.

  A short freeze of about 0.1 second every 15-20 seconds, even without moving:
  it comes from Virtual Desktop's wireless streaming, not from the game
  (measured on October 11, 2026: it also happens in the menu, and the graphics
  card isn't maxed out). In the VD app on the headset, try a lower bitrate
  ("Bitrate") than the maximum recommended above, or another codec (AV1 on
  RTX 40/50 cards, or HEVC 10-bit), and keep the headset close to the Wi-Fi
  router (5 or 6 GHz), with no wall in between.

-------------------------------------------------------------------------------
7. TROUBLESHOOTING
-------------------------------------------------------------------------------
  - Nothing in the headset: Virtual Desktop must be connected BEFORE the level.
  - Blurry image / "Low resolution image" in the headset: the game was started
    before the headset was connected (the VR resolution is only chosen when the
    game starts). Connect the headset (Virtual Desktop or Quest Link), then
    restart Teardown.
    In the panel's Info tab: "Headset not detected" = the OpenXR runtime isn't
    responding.
  - "Lua mod: no response": the mod isn't enabled (step 2.3) or isn't in the
    game's "mods\teardownvr" folder. Run "Installer Teardown VR.exe" again.
  - After a Teardown update, if the Info tab says
    "Synchronized stereo: unavailable", the mod falls back to alternating eyes.
  - Logs: run "Installer Teardown VR.exe" and click "Prepare my logs". A file
    TeardownVR_journaux_<date>.zip appears on your Desktop. It contains the
    mod's logs, your VR settings and a description of your PC (Windows,
    processor, memory, graphics card); nothing is sent automatically.
    The logs of your last 10 sessions are also kept in the game folder, in
    "TeardownVR_journaux" (the current or last session: TeardownVR.log, next to
    it).

-------------------------------------------------------------------------------
8. UNINSTALLING
-------------------------------------------------------------------------------
  Run "Installer Teardown VR.exe" and choose "Uninstall Teardown VR". The game
  goes back to normal (your TeardownVR.ini settings are kept, delete them if
  you want).

-------------------------------------------------------------------------------
Teardown VR is a free, unofficial fan mod, provided as is, without any
warranty and without support. A Teardown update can break it at any time; no
fix or update is promised. It is not affiliated with or endorsed by Tuxedo
Labs. Source code (GPL-3.0): its address is in LICENCES.txt, which comes with
the installer.
