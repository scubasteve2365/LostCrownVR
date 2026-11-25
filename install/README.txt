Prince of Persia: The Lost Crown VR
VR mod for Prince of Persia: The Lost Crown (Steam).
Native stereo 3D, played with a gamepad, as a diorama floating in front of you.
This mod is free. If you enjoy it, you can buy me a coffee: https://buymeacoffee.com/scubasteve2365

You need
- The game on Steam, with your Ubisoft account signed in (the game needs it).
- A PC VR headset with an OpenXR runtime. Tested: SteamVR on a Pimax Dream Air (set SteamVR as your OpenXR runtime in SteamVR's settings).
- A gamepad (Xbox type).

Install
1. Unzip this folder anywhere.
2. Double click "Install LostCrownVR.bat" and type 1.
If another mod already uses winhttp.dll in the game folder (BepInEx, which PoPTLCFix uses), the installer renames it to winhttp_chain.dll and this mod loads it too, so it stays installed. Note: on the current game version BepInEx cannot load its plugins at all (the game's copy protection stops it), with or without this mod.

Play
1. Start SteamVR (or your headset's software).
2. Start the game from Steam as usual. VR starts by itself at the title screen.
3. Load a save. A stage floats in front of you, with the game's HUD on it.

Settings
Hold both sticks in for half a second to open the VR settings menu, or press Insert on the keyboard. The game pauses and gets no input while it is open. LB and RB (Page Up and Page Down) switch between the Stage, Picture and Menu tabs. Up and down choose a setting, left and right change it, A (Enter) selects, B (Esc) closes. Every row explains itself at the bottom of the menu.
Stage
- Stage distance and Stage size: where the stage floats and how big it is.
- Depth: how strong the 3D is. Higher makes the world feel deeper and smaller. If far backgrounds strain your eyes, lower it or raise Pop-out.
- Pop-out: brings the Prince and more of the scene out of the stage toward you, and eases far backgrounds. 0% keeps the Prince on the stage. Default 100%.
- Edge match: at the left and right edges of the stage, each eye shows only the part of the world both eyes can see, near or far. What only one eye could see is hidden in the Surround colour. On by default.
- Recenter: puts the stage and the menu in front of you again. Your headset's own recenter does the same.
Picture
- Resolution: 100% matches your headset's pixel density across the stage. Takes effect the next time you start the game.
- Sharpen: crisper edges. 0% turns it off. Default 100%.
- Picture: Auto picks for your headset software: Steady on Virtual Desktop, Floating Panel on everything else. Steady draws the stage into each eye's view; Floating Panel can look a little sharper.
- Surround: what you see around the stage. Black, Green (for Virtual Desktop's chroma-key passthrough) or Transparent (your room, where the headset software offers it).
Menu
- Menu size, Menu height, Menu distance, Reset to defaults, Close.

While VR runs the mod turns off the game's VSync and frame limit (it paces the game to your headset), and the screen effects that look wrong in VR: bloom, vignette, chromatic aberration, depth of field, motion blur, film grain and screen-space shading. Cutscenes keep their 3D depth on what the cutscene camera looks at. Menus such as the pause and character screens show flat on the stage.

Uninstall
Double click "Install LostCrownVR.bat" and type 2. No game files are changed by this mod; uninstall puts the game folder back as it was (and puts back another mod's winhttp.dll if the installer had renamed it).

Performance tips
- SteamVR: in Settings, Video, Per-application video settings, set the game's Throttling Behavior to Fixed at your refresh rate, and Motion Smoothing off.
- The game is light: 72 or 90 Hz is easy on most PCs.

Good to know
- The game needs your Ubisoft account signed in. The mod blocks the game's own web requests that go through the game's code, but Ubisoft Connect's own connection is outside the game and is not touched. Don't use online features with the mod.
- The mod's 3D is made for the game's side-on view. Some cutscene camera moves can still change the sense of scale for a moment.
- No motion controls. A gamepad is required.
- Settings are saved in LostCrownVR\LostCrownVR.ini in the game folder. Please attach LostCrownVR.log (in the game folder) when you report a problem.

If something goes wrong
- The game stays flat: open the menu (both sticks or Insert). If VR did not start, it says why. Check your OpenXR runtime is set. If no runtime is set and SteamVR is installed, the mod uses SteamVR. Then look in LostCrownVR.log in the game folder.
- The game window on your monitor stays black or frozen while VR runs: that is normal, the picture goes to the headset.
- The game does not start and nothing appears: Ubisoft Connect sometimes stops while starting the game (its log says "Cannot switch modes after logging in"). This is Ubisoft's launcher, not the mod. Wait a minute and start the game again.
- Windows warns about the bat file: the mod is not signed, so Windows may ask you to confirm. The bat is plain text; read it first if you like.
- Smart App Control: if it is on, Windows can block unsigned mod DLLs and the game runs flat. There is no workaround in the mod.
- When SteamVR starts, its dashboard may appear in front of the stage. Close it as usual.

Tested on
SteamVR on the author's PC (Pimax Dream Air), NVIDIA graphics. Meta Quest 3 through Virtual Desktop, other headsets, AMD and Intel graphics have not been tried yet.

Support: https://buymeacoffee.com/scubasteve2365

Version 1.0. Credits and licences are in THIRD-PARTY-NOTICES.txt. MIT licensed, by scubasteve2365.
This is a fan mod, not made or approved by Ubisoft.
