# The chardev O_NONBLOCK semantics: reads capped at one 32-byte
# buffer, and EAGAIN rather than waiting on a concurrent reader.

import os
import threading
import time

fd = os.open("/dev/jitterentropy", os.O_RDONLY | os.O_NONBLOCK)

# The instance's self test takes its lock too: retry an EAGAIN.
deadline = time.monotonic() + 10
while True:
    try:
        data = os.read(fd, 4096)
        break
    except BlockingIOError:
        assert time.monotonic() < deadline, "no read succeeded"
        time.sleep(0.01)
assert len(data) == 32, f"nonblocking read returned {len(data)} bytes"

# The instance and its lock are per open, so the second reader
# shares this fd and its O_NONBLOCK. It holds the lock for most
# of each 32-byte chunk and re-issues its reads, so the poller
# mostly sees EAGAIN; the loop below tolerates winning the gap.
# The sleep lets the first read block before O_NONBLOCK is set.
stop = threading.Event()
failure = []


def reader():
    try:
        while not stop.is_set():
            try:
                os.read(fd, 4096)
            except BlockingIOError:
                pass
    except Exception as e:
        failure.append(e)


os.set_blocking(fd, True)
t = threading.Thread(target=reader, daemon=True)
t.start()
time.sleep(0.5)
os.set_blocking(fd, False)

deadline = time.monotonic() + 10
while True:
    assert t.is_alive(), f"reader ended: {failure}"
    try:
        os.read(fd, 16)
    except BlockingIOError:
        break
    assert time.monotonic() < deadline, "no EAGAIN observed"
    time.sleep(0.01)
stop.set()
t.join(10)
assert not failure, failure
print("OK")
