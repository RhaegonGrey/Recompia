# source-sha256: 5cbb87d01e3e7fefcc22735bcc5a27585b9aca77f56d6fbef640c1799de09fb6
यह अंग्रेज़ी README का मशीनी अनुवाद है, इसमें गलतियाँ हो सकती हैं, और अंग्रेज़ी README ही आधिकारिक है।

Pokopia PC @VERSION@ Linux, SteamOS और Steam Deck के लिए

1. आपको एक Vulkan ड्राइवर चाहिए: Mesa के AMD और Intel ड्राइवर या NVIDIA का proprietary ड्राइवर। इसके अलावा कुछ नहीं, क्योंकि गेम, रनटाइम और शेडर ट्रांसलेटर सभी Linux बाइनरी हैं।
2. PokopiaLauncher.AppImage चलाएँ और अपना डंप फ़ोल्डर चुनें (base/ और updates/<version>/, दोनों में exefs/ और romfs/) या कोई .nup फ़ाइल। संस्करण 2.0.1a आवश्यक है।
3. Set up दबाएँ। पहली बार यह अपने बिल्ड टूल डाउनलोड करता है (Python और Zig, लगभग 100 MB), फिर इसी PC पर गेम को कंपाइल करता है और उसके शेडर का अनुवाद करता है। आप रोककर बाद में जारी रख सकते हैं।
4. Play दबाएँ। गेम मूल रूप से चलता है: न Proton, न Wine। FSR 3 और DLSS अपस्केलर केवल Windows के लिए हैं।

अगर PokopiaLauncher.AppImage शुरू न हो (ऐसा सिस्टम जिसमें FUSE नहीं है): ./PokopiaLauncher.AppImage --appimage-extract-and-run
Steam से (Deck पर गेम मोड): PokopiaLauncher.AppImage को non-Steam गेम के रूप में जोड़ें और लॉन्च विकल्पों में --play लिखें।

अपडेट: नया रिलीज़ अपनी इंस्टॉल पर एक्सट्रैक्ट करके ऊपर लिखें। अगर अपडेट गेम कोड बदलता है, तो लॉन्चर बताता है: फिर से Set up दबाएँ और यह pkp दोबारा कंपाइल करता है। आपके शेडर और सेव रहते हैं।

कम्युनिटी और सहायता: https://discord.recompia.com

लाइसेंस: LICENSE.txt और licenses/.
