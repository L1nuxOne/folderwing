#!/usr/bin/env python3
"""Force the real helper's old-kernel path; filter lives only in this child."""
import ctypes
import errno
import os
import platform
import sys

if platform.machine() != 'x86_64':
    raise SystemExit('openat2 seccomp fixture requires x86_64')

class Filter(ctypes.Structure):
    _fields_ = [('code', ctypes.c_ushort), ('jt', ctypes.c_ubyte),
                ('jf', ctypes.c_ubyte), ('k', ctypes.c_uint)]

class Program(ctypes.Structure):
    _fields_ = [('length', ctypes.c_ushort), ('filters', ctypes.POINTER(Filter))]

# Load seccomp_data.nr; return ENOSYS for openat2 (437), allow other calls.
filters = (Filter * 4)(Filter(0x20, 0, 0, 0), Filter(0x15, 0, 1, 437),
                       Filter(0x06, 0, 0, 0x50000 | errno.ENOSYS),
                       Filter(0x06, 0, 0, 0x7fff0000))
program = Program(4, filters)
libc = ctypes.CDLL(None, use_errno=True)
if libc.prctl(38, 1, 0, 0, 0) or libc.prctl(22, 2, ctypes.byref(program), 0, 0):
    raise OSError(ctypes.get_errno(), 'install private openat2 filter')
helper = os.environ['OPENPILOT_TEST_MOUNT_HELPER']
os.execv(helper, [helper, *sys.argv[1:]])
