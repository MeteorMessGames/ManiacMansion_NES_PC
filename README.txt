MANIAC MANSION (NES prototype) - MOUSE EDITION
Player guide
==============================================================================

GETTING STARTED
---------------
You need your own copy of the Maniac Mansion NES prototype ROM; it is not
included. Put it in this folder named exactly:

    Maniac Mansion (Prototype).nes

then double-click ManiacMansion.exe.
Only that exact prototype ROM works; any other file is refused.

1. Press Enter (or right-click) to skip the logo and intro screens.
2. On the character screen, click two kids to go with Dave, then click START.
3. Press Enter once to skip Dave's speech in the front yard. You are now
   in control.

Goal: get into the mansion, find your way around, and rescue Sandy from
Dr. Fred. Use your kids together: some puzzles need two or three of them.


MOUSE CONTROLS
--------------
Left click     Walk to a spot, pick a verb, or pick an item.
               Clicking an object with no verb picked walks to it (GO TO).

Right click    Do the obvious thing with the object under the cursor:
               GET things you can take, OPEN or CLOSE doors, READ signs,
               PUSH or PULL things, TURN ON or OFF, USE, and so on.
               While you hover an object, the sentence line shows what a
               right click will do (for example "READ SIGN").
               Right click also skips cutscenes.

Middle click   In-game save menu: opens the game's own menu (Back to the
               Mansion / Save game / Restore saved game / New game), same as
               Start on the NES pad. Middle click again inside that menu goes
               back to the game.

Mouse wheel    Scroll the inventory.

Two-step commands still work the classic way: left click a verb
(for example USE), then left click the object(s).

Skipping text: left click above the room (the text lines) or during a
cutscene to skip the current message. On the logo and title screens a left
click skips too.


CONTROLLER
----------
D-pad / stick  Move the cursor (speed: pause menu -> Options -> Key Speed)
A              Left click
B              Right click (context verb; skips cutscenes)
X              Cycle the verb (Select)
Y              Skip the current message
Start / Select Pause menu (also Back, Guide / Home; D-pad moves, A
               confirms, B goes back)
Left shoulder  The game's own menu (save / restore)


KEYBOARD
--------
Esc            Pause menu (see below)
Enter / Space  Skip a cutscene
F9             Switch the picture filter: Sharp / CRT / CRT Strong
F11            Fullscreen on / off
F12            Save a screenshot (cyc_shot_0000.png etc. in the game folder)
Tab            Cycle through the verbs
Z              Same as a left click (A button)
Arrow keys     NES d-pad


PAUSE MENU (Esc)
----------------
The game is paused (and silent) while the menu is open. Use the mouse, or Up/Down and
Enter. Esc or right click goes back.

  Resume Game   Back to the game.
  Save State    Pick one of 10 slots (scroll the list with the mouse wheel
                or Up/Down). Each slot shows a screenshot and the time it
                was saved. Saving over a used slot asks you to click a
                second time first.
  Load State    Pick a slot to load. The screenshot on the right shows what
                you will get.
  Options       Music volume, Sound FX volume, Fullscreen, Filter, Scale,
                Shape, Mouse Speed and Key Speed.
                Volumes: drag or click the bar, or Left / Right in 5% steps.
  Exit Game     Asks first: Save to a slot and exit, Exit without saving, or
                Cancel. Closing the window asks the same thing.


METEOR MESS
-----------
Switching on the Meteor Mess arcade machine plays a short chiptune jingle over
its high-score screen.


SAVING
------
There are two kinds of saves:

1. Save states (pause menu, 10 slots). Instant, any time, anywhere.
   Files: ManiacMansion.slot1.state ... slot6.state, next to the game.
   NOTE: a save state only loads in the exact build that made it. After an
   update, old save states may say "from a different version".

2. The game's own save (middle click -> Save game). This is the cartridge
   battery save and it survives updates. It is also copied into the
   save-state slot you last saved to this session (the first time in a
   session it goes to an empty slot); a banner "SAVED TO SLOT N" confirms
   it. "Restore saved game" in that menu loads the latest in-game save
   of the session.

Tip: save often and use different slots before risky moves. In Maniac
Mansion kids can die, and some mistakes make the game unwinnable.


OPTIONS
-------
Music and Sound FX volume (pause menu -> Options)
  Separate levels for the music and for sound effects (the intro whoosh,
  doorbell, and so on), 0-100%.

Filter (F9)
  Sharp        Crisp, pixel-perfect: every NES pixel the same size, using the
               largest whole scale that fits (6x on a 1440p screen, 4x on
               1080p with thin black bars above and below). Default.
  CRT          Old TV look: clear scanlines that widen on bright colors, an
               RGB stripe mask and a soft glow, like a good home TV.
  CRT Strong   Thinner scanlines with darker gaps and a bit more glow, like
               an arcade monitor. Looks best at larger sizes (5x, 6x).
               Both CRT filters run on the graphics card (OpenGL); without
               OpenGL they fall back to the CPU, which is slower.

Scale (pause menu -> Options)
  Auto         Sharp: the largest whole scale that fits (default).
               CRT: fills the screen.
  2x - 6x      A fixed size, e.g. 4x on a 1440p screen for a smaller,
               centered picture. Click or Enter steps through the sizes,
               Left/Right go up and down. Only sizes that fit your screen
               are offered; if a saved size doesn't fit (e.g. 6x on a 1080p
               screen), Auto is used. The pause menu itself always fills
               the screen.

Shape (pause menu -> Options)
  Square       Every NES pixel is square (default).
  TV (4:3)     The shape the game had on a real TV: NES pixels were a bit
               wider than tall (8:7), so the picture is 4:3. With the Sharp
               filter, pixels stay crisp (only a one-pixel soft edge where
               two of them meet, so no uneven columns).

Fullscreen (F11)
  Uses your monitor's own resolution (1080p, 1440p, 4K ...).

Mouse Speed / Key Speed (pause menu -> Options)
  How fast the cursor moves with the mouse and with the arrow keys:
  50% to 300%, 100% is normal.

Skipping text
  Left click above the room (on the text lines) or during a cutscene to
  skip the current message.

Update check
  At launch the game asks GitHub once whether a newer release exists and
  shows a short notice if so. Nothing about you is sent. To turn it off,
  set UpdateCheck = 0 in config.ini.

Your volume, filter and scale choices are remembered (config.ini).


TIPS FOR NEW PLAYERS
--------------------
- Look at everything: hover objects to see their names on the sentence line.
- Right click is a quick shortcut, but some puzzles need a specific verb.
  If right click does not do what you want, pick the verb yourself.
- Check the door mat by the front door.
- Use NEW KID to switch between your kids.


FILES IN THIS FOLDER
--------------------
ManiacMansion.exe            The game
Maniac Mansion (Prototype).nes   Your ROM (you provide it)
config.ini                   Settings (window size, filter, volumes ...)
saves\                       The game's own battery save
ManiacMansion.slotN.state    Save-state slots (+ .thumb screenshots)
mouse_debug.log              Log for bug reports
README.txt                   This guide
LICENSE.txt, licenses\       License terms for this program and the parts
                             it is built from
(config.ini, saves, state slots and the log are created when you play)
