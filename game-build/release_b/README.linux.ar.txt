# source-sha256: 5cbb87d01e3e7fefcc22735bcc5a27585b9aca77f56d6fbef640c1799de09fb6
هذه ترجمة آلية لملف README الإنجليزي، وقد تحتوي على أخطاء. النسخة الإنجليزية هي المرجع.

Pokopia PC @VERSION@ لنظام Linux وSteamOS وSteam Deck

1. تحتاج إلى مشغّل Vulkan: مشغّلات Mesa لـ AMD وIntel أو مشغّل NVIDIA الخاص. لا شيء غير ذلك، لأن اللعبة وزمن التشغيل ومترجم الشيدرات كلها برامج Linux أصلية.
2. شغّل PokopiaLauncher.AppImage واختر مجلد النسخة المستخرجة (base/ و updates/<version>/، وكل منهما يحتوي exefs/ و romfs/) أو ملف .nup. الإصدار 2.0.1a مطلوب.
3. اضغط Set up. في المرة الأولى ينزّل أدوات البناء (Python وZig، نحو 100 ميغابايت)، ثم يبني اللعبة ويترجم شيدراتها على هذا الحاسوب. يمكنك الإيقاف المؤقت والمتابعة لاحقاً.
4. اضغط Play. تعمل اللعبة أصلياً: بدون Proton وبدون Wine. موسّعات FSR 3 و DLSS خاصة بـ Windows فقط.

إذا لم يبدأ PokopiaLauncher.AppImage (نظام بدون FUSE): ./PokopiaLauncher.AppImage --appimage-extract-and-run
من Steam (وضع الألعاب على Deck): أضف PokopiaLauncher.AppImage كلعبة غير تابعة لـ Steam مع خيارات التشغيل --play.

التحديث: استخرج الإصدار الجديد فوق تثبيتك. إذا غيّر التحديث شيفرة اللعبة، يخبرك المشغّل: اضغط Set up مرة أخرى وسيعيد بناء pkp. تبقى شيدراتك وملفات حفظك.

المجتمع والمساعدة: https://discord.recompia.com

التراخيص: LICENSE.txt و licenses/.
