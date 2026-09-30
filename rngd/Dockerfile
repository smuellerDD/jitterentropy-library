# The build context is the root of the source tree:
#	docker build -f rngd/Dockerfile .
FROM alpine

RUN apk add --no-cache cmake gcc libc-dev linux-headers make

WORKDIR /source
COPY . .

# Static, so that the second stage needs nothing but the daemon.
RUN cmake -S . -B /build -DENABLE_RNGD=ON -DBUILD_SHARED_LIBS=OFF \
	-DBUILD_TESTING=OFF -DENABLE_TOOLS=OFF -DINSTALL_MAN=OFF && \
    cmake --build /build --target jitterentropy-rngd

FROM alpine

COPY --from=0 /build/rngd/jitterentropy-rngd /usr/bin

# -F: the daemon is the container's main process and must not detach.
ENTRYPOINT ["/usr/bin/jitterentropy-rngd"]
CMD ["-F", "-v"]
