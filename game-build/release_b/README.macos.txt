Pokopia PC @VERSION@ for Apple Silicon Macs

1. You need an M1 or newer Mac (arm64, macOS 13 or newer). Nothing else: the game, the runtime and the shader translator are Mac programs, and Vulkan comes from MoltenVK, which this release carries.
2. Open PokopiaLauncher.app and pick your dump folder (base/ and updates/<version>/, each with exefs/ and romfs/) or a .nup. Version 2.0.1a is required.
3. Press Set up. The first time it downloads its build tools (Python, about 25 MB), then compiles the game and translates its shaders on this Mac. You can pause and continue later.
4. Press Play. The game runs natively: no Proton, no Wine, and no Vulkan driver to install.

The first start of the app: macOS blocks it because it is not notarized. Right-click PokopiaLauncher.app, choose Open, then Open again (or System Settings > Privacy & Security > Open Anyway).

Updating: extract the new release over your install. If the update changes the game code, the launcher says so: press Set up again and it rebuilds pkp. Your shaders and saves are kept.

Community and help: https://discord.recompia.com

Licences: LICENSE.txt and licenses/.
