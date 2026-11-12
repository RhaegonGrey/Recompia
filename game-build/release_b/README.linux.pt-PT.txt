# source-sha256: 5cbb87d01e3e7fefcc22735bcc5a27585b9aca77f56d6fbef640c1799de09fb6
Esta é uma tradução automática do texto em inglês; pode conter erros. O texto em inglês é a versão oficial.

Pokopia PC @VERSION@ para Linux, SteamOS e Steam Deck

1. Precisas de um controlador Vulkan: os controladores Mesa para AMD e Intel ou o proprietário da NVIDIA. Nada mais, porque o jogo, o runtime e o tradutor de shaders são binários de Linux.
2. Executa o PokopiaLauncher.AppImage e escolhe a pasta do teu dump (base/ e updates/<version>/, cada uma com exefs/ e romfs/) ou um ficheiro .nup. É necessária a versão 2.0.1a.
3. Carrega em Set up. Da primeira vez descarrega as ferramentas de compilação (Python e Zig, cerca de 100 MB) e depois compila o jogo e traduz os shaders neste PC. Podes pausar e continuar mais tarde.
4. Carrega em Play. O jogo corre nativamente: sem Proton e sem Wine. Os upscalers FSR 3 e DLSS são só para Windows.

Se o PokopiaLauncher.AppImage não arrancar (um sistema sem FUSE): ./PokopiaLauncher.AppImage --appimage-extract-and-run
A partir do Steam (modo Jogo no Deck): adiciona o PokopiaLauncher.AppImage como um jogo que não é do Steam e escreve --play nas opções de arranque.

Para atualizar, extrai a nova versão por cima da tua instalação. Se a atualização mudar o código do jogo, o launcher avisa: carrega em Set up outra vez e ele recompila o pkp. Os teus shaders e jogos guardados mantêm-se.

Comunidade e ajuda: https://discord.recompia.com

Licenças: LICENSE.txt e licenses/.
