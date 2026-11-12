// SPDX-License-Identifier: GPL-3.0-or-later
// <mstcpip.h> for the POSIX build: SIO_UDP_CONNRESET (src/os/compat/winsock2.h). BSD has no equivalent -- an ICMP
// port-unreachable does not surface on the socket -- so the ioctl is a successful no-op.
#pragma once
#include "winsock2.h"
