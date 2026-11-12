# source-sha256: 5cbb87d01e3e7fefcc22735bcc5a27585b9aca77f56d6fbef640c1799de09fb6
Esta é uma tradução automática do texto em inglês; ela pode conter erros. O texto em inglês é a versão oficial.

Pokopia PC @VERSION@ para Linux, SteamOS e Steam Deck

1. Você precisa de um driver Vulkan: os drivers Mesa para AMD e Intel ou o proprietário da NVIDIA. Nada além disso, porque o jogo, o runtime e o tradutor de shaders são binários de Linux.
2. Execute PokopiaLauncher.AppImage e escolha a pasta do seu dump (base/ e updates/<version>/, cada uma com exefs/ e romfs/) ou um arquivo .nup. A versão 2.0.1a é necessária.
3. Pressione Set up. Na primeira vez ele baixa as ferramentas de compilação (Python e Zig, cerca de 100 MB) e depois compila o jogo e traduz os shaders neste PC. Você pode pausar e continuar depois.
4. Pressione Play. O jogo roda nativamente: sem Proton, sem Wine. Os upscalers FSR 3 e DLSS são exclusivos do Windows.

Se o PokopiaLauncher.AppImage não iniciar (um sistema sem FUSE): ./PokopiaLauncher.AppImage --appimage-extract-and-run
Do Steam (modo Jogo no Deck): adicione PokopiaLauncher.AppImage como um jogo que não é do Steam e use --play nas opções de inicialização.

Para atualizar, extraia a nova versão sobre a sua instalação. Se a atualização mudar o código do jogo, o launcher avisa: pressione Set up de novo e ele recompila o pkp. Seus shaders e saves são mantidos.

Comunidade e ajuda: https://discord.recompia.com

Licenças: LICENSE.txt e licenses/.
