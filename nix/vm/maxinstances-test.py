# max_instances: opens beyond it fail with ENFILE and take no slot
# or count, a close frees one, root is exempt.

import errno
import json
import os
import sys

# The module's max_instances; if 0, argv[2] is the open count.
limit = int(sys.argv[1])
opens = limit if limit else int(sys.argv[2])
privileged = os.geteuid() == 0


def chardev():
    """The counters, or None where the caller may not read them."""
    try:
        with open("/proc/jitterentropy/statistics") as f:
            return json.load(f)["charDevice"]
    except PermissionError:
        return None


# The statistics document is root only.
before = chardev()
assert (before is not None) == privileged, before

fds = [os.open("/dev/jitterentropy", os.O_RDONLY)
       for _ in range(opens)]

if before is not None:
    admitted = chardev()
    assert admitted["openInstances"] == \
        before["openInstances"] + opens, admitted
    assert admitted["cumulativeOpens"] == \
        before["cumulativeOpens"] + opens, admitted

if limit and privileged:
    # CAP_SYS_RESOURCE is exempt from the cap.
    fds += [os.open("/dev/jitterentropy", os.O_RDONLY)
            for _ in range(2)]
elif limit:
    try:
        os.close(os.open("/dev/jitterentropy", os.O_RDONLY))
    except OSError as e:
        assert e.errno == errno.ENFILE, f"errno {e.errno}"
    else:
        raise AssertionError("open beyond max_instances succeeded")

    # Closing one frees exactly one slot.
    os.close(fds.pop())
    fds.append(os.open("/dev/jitterentropy", os.O_RDONLY))

for fd in fds:
    os.close(fd)
if before is not None:
    assert chardev()["openInstances"] == \
        before["openInstances"], chardev()
print("OK")
