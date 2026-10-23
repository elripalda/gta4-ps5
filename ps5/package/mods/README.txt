GTA IV for PS5 - optional Xbox 360 mods

Put replacement game files in mods/game, using the same relative path as the
installed file in UserData/LibertyRecomp/game. Enable Mods on the hub's Game page,
then start the game. Quit and restart the app after changing mod files.

For a script menu, use an Xbox 360 archive pack for the USA game with Title
Update 8 and the episode you are playing. Keep the pack's bootstrap/loader
inside its script archive. A loose .sco does not start automatically.
Find the original script.img/script_network.img under UserData/LibertyRecomp/game
and mirror that path below mods/game. If it lives under game/update, put
the replacement under mods/game/update too.

PC .asi plugins, ScriptHook DLLs and .NET scripts do not run here. PS3 menus
and native Xbox 360 executables are not compatible replacements either.
Only files opened through the game's filesystem can be replaced; this
folder does not inject loose files into the contents of IMG/RPF archives.
Native executables, keys, manifests, hidden files, README files and symlinks
are ignored. Filename collisions that differ only by case are skipped.

Use one menu pack at a time. Its controller shortcut is set by the menu.
Disable Mods and restart the app to return to the installed game.
The original files are never overwritten. A replacement xbox360.rpf takes
priority over the built-in PlayStation button prompts.

Specific menu compatibility still needs testing on a PS5. No mod or game
files are included with the app.
