# max_kcapi_instances: the same for AF_ALG binds, counted apart from
# the character device.

import errno
import os
import socket
import sys

# The module's max_kcapi_instances; if 0, argv[2] is the bind
# count.
limit = int(sys.argv[1])
binds = limit if limit else int(sys.argv[2])
privileged = os.geteuid() == 0


def instantiate():
    """A socket bound to jitter_rng: the bind allocates the tfm."""
    s = socket.socket(socket.AF_ALG, socket.SOCK_SEQPACKET, 0)
    try:
        s.bind(("rng", "jitter_rng"))
    except BaseException:
        s.close()
        raise
    return s


def generate(s):
    op, _ = s.accept()
    with op:
        data = op.recv(32)
    assert len(data) == 32, f"read {len(data)} bytes"


def refused():
    try:
        instantiate().close()
    except OSError as e:
        assert e.errno == errno.ENFILE, f"errno {e.errno}"
    else:
        raise AssertionError(
            "bind beyond max_kcapi_instances succeeded")


tfms = [instantiate() for _ in range(binds)]
generate(tfms[0])
generate(tfms[-1])

if limit and privileged:
    # CAP_SYS_RESOURCE is exempt from the cap.
    tfms += [instantiate() for _ in range(2)]
    generate(tfms[-1])
elif limit:
    refused()

    # A refused bind took no slot; closing one frees exactly one.
    tfms.pop().close()
    tfms.append(instantiate())
    generate(tfms[-1])
    refused()

    # A full crypto API count leaves the character device open.
    fd = os.open("/dev/jitterentropy", os.O_RDONLY)
    assert len(os.read(fd, 32)) == 32
    os.close(fd)

for s in tfms:
    s.close()
print("OK")
