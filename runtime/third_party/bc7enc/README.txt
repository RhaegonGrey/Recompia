bc7enc: a portable BC7 encoder (Richard Geldreich, Jr.), MIT license or public domain (see the end of bc7enc.c).

Vendored at f66c2e4 (github.com/richgel999/bc7enc): bc7enc.c, bc7enc.h, rgbcx.h (its single-header RDO/BC1-5 helper, kept
because bc7enc.c includes it).

Why: astc2bc7 (tools/textures) is an x86 ISPC build, so the POSIX runtime had no BC7 encoder (a2b_encode_bc7 returned
unsupported) and every BC7 texture patch failed -- the keyboard button prompts could not be drawn into the game's atlas
("kbm prompt A: the atlas patch failed"). os_posix_a2b.cpp now encodes with this instead: mode 6 only, which is what the
glyphs need (flat colours and alpha), at ~20 ms for an 84x84 rectangle.
