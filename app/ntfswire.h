/* Local, authenticated NTFS worker protocol. No arbitrary commands or raw IO. */
#pragma once
#include "spacewire.h"
#define NTN_MAGIC 0x4e544653u
#define NTN_VERSION 1u
#define NTN_PIPE_PREFIX L"\\\\.\\pipe\\OpenPilot.Ntfs."
#define NTN_REQUEST 100u
#define NTN_CONTROL 101u
#define NTN_PAUSE 1u
#define NTN_RESUME 2u
#define NTN_STOP 3u
#define NTN_ERROR 102u
#define NTN_TIMEOUT 180000u
