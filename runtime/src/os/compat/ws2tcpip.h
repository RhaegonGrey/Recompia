// SPDX-License-Identifier: GPL-3.0-or-later
// <ws2tcpip.h> for the POSIX build: getaddrinfo and the inet_* conversions are the BSD ones.
#pragma once
#include "winsock2.h"

// Winsock spells the AI_* flags with a WS prefix in the older headers; both names are accepted here.
#define AI_PASSIVE_ AI_PASSIVE
