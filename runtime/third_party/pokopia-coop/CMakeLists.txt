# pokopia-coop: pokopia-native's Link Play module (MIT, windows/src/coop @9cb7f07, THIRD_PARTY_NOTICES.md), vendored unchanged but for one
# header line per file, the relay UDP extension in coop_int.h / coop_mpnet.c and the LAN sweep / mDNS responder for Windows without DNS-SD (Wine,
# Proton) in coop.h / coop_int.h / coop_ldn.c / coop_plat.c / coop_disc.c (all marked "pokopia-recomp extension"). Portable C11; the ldn and bsd services (src/services/ldn.cpp, network.cpp) call it for [net] protocol = native.
# Its own tests (tests/coop_test.c) and the relay tool are not vendored.
add_library(pokopia_coop STATIC coop_ldn.c coop_vnet.c coop_mpnet.c coop_plat.c coop_disc.c coop_auth.c coop_interact.c coop_nat.c)
target_include_directories(pokopia_coop PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(pokopia_coop PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(pokopia_coop PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
if(WIN32)
    target_link_libraries(pokopia_coop PUBLIC ws2_32 ole32 iphlpapi)   # ole32: network profiles, iphlpapi: addresses (coop_plat.c)
endif()
# macOS: coop_plat.c's POSIX branch uses BSD sockets and pthreads, which live in libSystem (docs/macos-port.md D6).
