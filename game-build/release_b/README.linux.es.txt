# source-sha256: 5cbb87d01e3e7fefcc22735bcc5a27585b9aca77f56d6fbef640c1799de09fb6
Esta es una traducción automática del texto en inglés; puede contener errores. El texto en inglés es la versión definitiva.

Pokopia PC @VERSION@ para Linux, SteamOS y Steam Deck

1. Necesitas un controlador Vulkan: los controladores Mesa para AMD e Intel o el propietario de NVIDIA. Nada más, porque el juego, el runtime y el traductor de sombreadores son binarios de Linux.
2. Ejecuta PokopiaLauncher.AppImage y elige la carpeta de tu volcado (base/ y updates/<version>/, cada una con exefs/ y romfs/) o un archivo .nup. Se necesita la versión 2.0.1a.
3. Pulsa Set up. La primera vez descarga las herramientas de compilación (Python y Zig, unos 100 MB) y después compila el juego y traduce sus sombreadores en este PC. Puedes pausar y continuar más tarde.
4. Pulsa Play. El juego se ejecuta de forma nativa: sin Proton y sin Wine. Los escaladores FSR 3 y DLSS son solo para Windows.

Si el archivo PokopiaLauncher.AppImage no se inicia en un sistema que no tiene FUSE: ./PokopiaLauncher.AppImage --appimage-extract-and-run
Para iniciarlo desde Steam (modo Juego en Deck): añade PokopiaLauncher.AppImage como un juego que no sea de Steam y escribe --play en las opciones de lanzamiento.

Para actualizar, extrae los archivos de la nueva versión sobre los de tu instalación. Si la actualización cambia el código del juego, el launcher te avisa: pulsa Set up otra vez para que vuelva a compilar pkp. Tus sombreadores y partidas guardadas se conservan.

Comunidad y ayuda: https://discord.recompia.com

Licencias: consulta LICENSE.txt y la carpeta licenses/.
