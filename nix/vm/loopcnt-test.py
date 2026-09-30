# The loop count bound JENT_IOCLOOPCNT enforces.

import errno
import fcntl
import os
import struct

# From jitterentropy_uapi.h: _IOW('J', 0x02, __u64).
JENT_IOCLOOPCNT = 0x40084A02
JENT_LOOPCNT_MAX = 1 << 16

fd = os.open("/sys/kernel/debug/jitter_rng/jent_raw_hires",
             os.O_RDONLY)

# 0 means the configured count.
for cnt in (0, 1, JENT_LOOPCNT_MAX):
    fcntl.ioctl(fd, JENT_IOCLOOPCNT, struct.pack("=Q", cnt))

for cnt in (JENT_LOOPCNT_MAX + 1, 1 << 20, (1 << 32) - 1,
            1 << 63):
    try:
        fcntl.ioctl(fd, JENT_IOCLOOPCNT, struct.pack("=Q", cnt))
    except OSError as e:
        assert e.errno == errno.EINVAL, f"{cnt}: errno {e.errno}"
    else:
        raise AssertionError(f"loop count {cnt} accepted")

os.close(fd)
print("OK")
