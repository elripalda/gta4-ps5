Place custom high-resolution texture packs here as <xxh64>.dds files
(subfolders are supported and scanned alphabetically).

How to install texture packs:
1. Directly on the PS5 (recommended):
   Copy PC GTA IV texture mods (.oiv, .zip, .img, .rpf, .wtd, .dds, or folders)
   into /data/homebrew/PPSA94004/mods/, then select Display -> Import Texture Mods
   in the PS5 Hub. Converted 2048px-capped packs are written here automatically.

2. Offline on a PC with tools/convert_texture_mod.py (optional, for .rar/.7z or
   custom --max-size):
     python3 tools/convert_texture_mod.py \
       --xbox xbox360.rpf --aes-key LibertyRecompLib/aes_key.bin \
       --mod /path/to/mod_folder --max-size 2048 --out ./textures/my_mod

3. Or toggle Debug -> Dump Textures in the hub to write captured Xbox 360
   textures into /data/homebrew/PPSA94004/textures_dump/<hash>.dds, replace any
   image while keeping the 16-hex-digit hash filename, and drop the .dds files here.

Tested Packs:
So far this feature has ONLY been tested with these 3 GTA IV texture packs:
- https://www.nexusmods.com/gta4/mods/105
- https://www.nexusmods.com/gta4/mods/282
- https://www.nexusmods.com/gta4/mods/357

Supported DDS formats: BC1 (DXT1), BC2 (DXT3), BC3 (DXT5), BC4, BC5, BC7,
RGBA8, BGRA8, BGR24, R8G8, R8 (2D textures, with or without mipmaps).
