Pokopia PC @VERSION@ for Linux, SteamOS and the Steam Deck

1. You need a Vulkan driver: Mesa's AMD and Intel drivers or NVIDIA's proprietary one. Nothing else, because the game, the runtime and the shader translator are Linux binaries.
2. Run PokopiaLauncher.AppImage and pick your dump folder (base/ and updates/<version>/, each with exefs/ and romfs/) or a .nup. Version 2.0.1a is required.
3. Press Set up. The first time it downloads its build tools (Python and Zig, about 100 MB), then compiles the game and translates its shaders on this PC. You can pause and continue later.
4. Press Play. The game runs natively: no Proton, no Wine. The FSR 3 and DLSS upscalers are Windows-only.

If PokopiaLauncher.AppImage does not start (a system without FUSE): ./PokopiaLauncher.AppImage --appimage-extract-and-run
From Steam (Game Mode on the Deck): add PokopiaLauncher.AppImage as a non-Steam game with the launch options --play.

Updating: extract the new release over your install. If the update changes the game code, the launcher says so: press Set up again and it rebuilds pkp. Your shaders and saves are kept.

Community and help: https://discord.recompia.com

Licences: LICENSE.txt and licenses/.
