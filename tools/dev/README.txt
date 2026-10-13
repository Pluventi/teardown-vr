TEARDOWN VR - DEVELOPMENT TOOLS (tools/dev)
===========================================

These are the helper scripts used while developing the mod: driving the test-only mock headset, measuring
captures, reading performance logs, checking the Lua mod, and finding the game's code again after a Teardown
update. Players do not need any of them. They are provided as is, without warranty and without support; they
were written for one development PC and may need adjusting on yours.

None of them contains game files, game code or game data. Some of them READ the running game (memory, logs,
script definitions) on your own PC. Their output (disassembly, captures, logs) must never be committed: the
.gitignore already ignores tools/dev/**/logs/, tools/dev/re/*.txt, tools/dev/perf/runs/, *.bmp and *.log.

Settings (environment variables)
--------------------------------
  TEARDOWN_DIR   the Teardown folder (the one with teardown.exe).
                 Default: C:\Program Files (x86)\Steam\steamapps\common\Teardown
  PYTHON         the Python interpreter used by the PowerShell scripts. Default: "python" from PATH.
  TDVR_MOCK_OUT  the mock headset's output folder. Default: build\Release\mock_out in the repository.
  TD_BB_W/TD_BB_H  the game's back-buffer size used by mockctl.py "point". Default: 1544 x 1600.
                 golevel2.ps1 and stress_run.ps1 set 1920 x 1080 for the commands they run.

Python 3.10 or newer. Packages: numpy, scipy and pillow (analysis), psutil (perf, re), capstone and pefile (re).

Warnings
--------
  - mock/cycle.ps1, mock/golevel2.ps1, analysis/cap.ps1 and perf/stress_run.ps1 CLOSE AND RESTART THE GAME and
    copy build\Release\dsound.dll and mod\TeardownVR\* into the game folder. Never run them while someone plays.
  - analysis/cap.ps1 and perf/stress_run.ps1 edit the game folder's TeardownVR.ini (stress_run.ps1 restores it
    afterwards, cap.ps1 does not): keep a copy of your own settings.
  - golevel2.ps1 snapshots the active mod list (%LOCALAPPDATA%\Teardown\modlists\1.xml) and puts it back if a
    click changed it.

mock/ - the test-only mock OpenXR headset
-----------------------------------------
Build it with  cmake --build build --config Release --target tdvr_mock_openxr  and point the mod to it with
[Debug] openxrRuntime=<repository>\build\Release\tdvr_mock_openxr.json  in TeardownVR.ini. The mock reads its
controls from mock_out\controls.txt and writes the headset's images there on request.
  mockctl.py         drive the mock: aim a controller at a menu pixel, the settings panel, the VR inventory or a
                     mod's floating panel, press buttons, move sticks and the head, capture the next headset frame
                     (see its header for the commands).
  aimfrac.py         aim the right hand at a fraction (u, v) of the virtual screen.
  findlastbutton.py  find the last button of the pause menu's bottom bar on a capture.
  cycle.ps1          close the game, keep its last TeardownVR.log in logs\, deploy the DLL and the Lua mod, start
                     the game (only when the OpenXR runtime reports a headset, for a real-headset ini).
  golevel2.ps1       restart the game and enter a sandbox level on the mock headset.
  goto_tool.ps1      switch tools until the wanted one is held, then print its shapes (uses re\snap_shapes.py).
  cycle_tools.ps1    switch through all tools, optionally capturing each.
  wait_idle.ps1      wait until the game is closed or the headset has been still for a few minutes.
  focus.ps1          bring the game window to the front.

analysis/ - image metrics on mock captures
------------------------------------------
  cap.ps1            capture the left eye with a given renderPreset / taaMode (edits the live ini).
  stereo_disp2.py    offset of a region of the left eye in the right eye (stereo disparity).
  ncc_tiles.py       stereo grain metric: per-tile normalised cross-correlation between the eyes.
  imgdiff.py         equivalence check between captures of different modes.
  sharpness.py       variance of the Laplacian on a few scene regions.
  taa_metrics2.py    anti-aliasing captures against a supersampled reference (PSNR, detail, halo).

perf/ - frame-time and CPU measurements
----------------------------------------
  parse_logs.py      summary of the frame statistics of TeardownVR.log files.
  analyze_run.py     per-phase analysis of a stress run (or of any TeardownVR.log), and comparisons of runs.
  thread_sampler.py  per-thread CPU of teardown.exe, sampled from outside the game (read-only).
  stress_inputs.py   fixed, timestamped input schedule for the mock headset stress test.
  stress_run.ps1     the CPU stress test on the mock headset (restarts the game; -IUserAgreed to confirm).
  run_matrix.ps1     interleaved stress runs over several settings.
  counters.txt       the Windows performance counters recorded by stress_run.ps1 (typeperf).

lua/
----
  luacheck.py        crude static checks of mod\TeardownVR\main.lua (block balance, local and upvalue limits,
                     unknown identifiers against the game's own data\script_defs.lua, read on your PC).

re/ - finding the game's code again after a Teardown update
-----------------------------------------------------------
Read-only helpers for the RUNNING teardown.exe (ReadProcessMemory; threadstate.py briefly suspends each thread
to read its registers). The game's code is encrypted on disk and decrypted by the game itself when it starts;
these tools only read the process memory on your own PC, change nothing and store nothing. Do not commit or
publish their output.
  tdmem.py           shared memory-reading helper (game and player pointers of Teardown 2.0.1).
  tddis.py           disassemble code at module offsets.
  bytes.py           print raw bytes at module offsets.
  findop.py          find instructions whose operands contain given text (e.g. a structure offset).
  findstr.py         find text in the image, or print the text stored at an offset.
  callers.py         find direct calls / jumps / RIP-relative references to given offsets.
  calls.py           list the calls, strings and branches of a function.
  iatrefs.py         find the code that calls given imports.
  vtref.py           find vtable slots that point to given offsets.
  memscan.py         find references to given strings and disassemble around them.
  threadstate.py     sample each thread's instruction pointer and return addresses.
  dbwin.py           capture OutputDebugString messages (the OpenXR loader logs there).
  snap_shapes.py     print the held tool's shapes (local transform, size, voxel scale).
  tool_box.py        print the held tool's bounding box.

shader_tools/
-------------
  gamedxc.py         compile an HLSL file with the shader compiler that ships with the game (dxcompiler.dll and
                     dxil.dll of the game folder). The game-derived VR shaders it was used for are not published
                     (see the CMake option TDVR_GAME_SHADERS).
