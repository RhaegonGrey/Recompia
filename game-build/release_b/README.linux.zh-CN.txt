# source-sha256: 5cbb87d01e3e7fefcc22735bcc5a27585b9aca77f56d6fbef640c1799de09fb6
这是英文文本的机器翻译，可能包含错误；英文文本是权威版本。

Pokopia PC @VERSION@，适用于 Linux、SteamOS 和 Steam Deck

1. 你需要一个 Vulkan 驱动：Mesa 的 AMD 和 Intel 驱动，或者 NVIDIA 的专有驱动。除此之外不需要别的，因为游戏、运行时和着色器翻译器都是 Linux 原生程序。
2. 运行 PokopiaLauncher.AppImage，选择你的转储文件夹（base/ 和 updates/<version>/，各自包含 exefs/ 和 romfs/）或一个 .nup 文件。需要版本 2.0.1a。
3. 点击 Set up。第一次会下载构建工具（Python 和 Zig，约 100 MB），然后在这台电脑上编译游戏并翻译它的着色器。你可以暂停，之后再继续。
4. 点击 Play。游戏原生运行：不需要 Proton，也不需要 Wine。FSR 3 和 DLSS 超分只支持 Windows。

如果 PokopiaLauncher.AppImage 无法启动（系统没有 FUSE）：./PokopiaLauncher.AppImage --appimage-extract-and-run
从 Steam 启动（Deck 上的游戏模式）：把 PokopiaLauncher.AppImage 添加为非 Steam 游戏，并在启动选项中填写 --play。

更新：把新版本解压覆盖到你的安装目录。如果更新改变了游戏代码，启动器会提示你：再点一次 Set up，它会重新编译 pkp。你的着色器和存档都会保留。

社区与帮助：https://discord.recompia.com

许可证：见 LICENSE.txt 和 licenses/。
