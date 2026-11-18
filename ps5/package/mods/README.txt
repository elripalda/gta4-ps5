Place PC GTA IV texture mods here (/data/homebrew/PPSA94004/mods/), then open
the PS5 Hub -> Display -> Import Texture Mods and press Cross.

Supported formats:
- .oiv (OpenIV packages)
- .zip archives
- .img / .rpf / .wtd files
- Loose .dds files or extracted mod folders
(Note: if a mod is packaged as .rar or .7z, extract it on a PC first and
copy the extracted folder or inner .oiv/.zip/.img/.wtd files here.)

What happens on import:
1. On first run, the installed Xbox 360 game textures are indexed and cached
   in textures/.xbox_index.bin (later imports reuse this cache).
2. Matching PC textures are converted into <xxh64>.dds packs under
   /data/homebrew/PPSA94004/textures/<mod_name>/, with resolutions capped at
   2048px max (via lossless mip skipping for PS5 memory stability).
3. Processed items are moved to mods/done/, and Custom Textures is turned On.

Tested Packs:
So far this converter has ONLY been tested with these 3 GTA IV texture packs:
- https://www.nexusmods.com/gta4/mods/105
- https://www.nexusmods.com/gta4/mods/282
- https://www.nexusmods.com/gta4/mods/357
