// SPDX-License-Identifier: GPL-3.0-or-later
// <bcrypt.h> for the POSIX build: BCryptGenRandom over arc4random_buf. The runtime uses it for the relay server's
// session tokens and for nn::csrng (services/misc.cpp), so it has to be a real CSPRNG, not a stand-in.
#pragma once
#include "nx_os.h"

typedef unsigned char UCHAR;
typedef UCHAR* PUCHAR;

#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002
#define BCRYPT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

#if defined(__cplusplus)
extern "C" {
#endif
NTSTATUS BCryptGenRandom(void* alg, PUCHAR buf, ULONG len, ULONG flags);
#if defined(__cplusplus)
}
#endif
