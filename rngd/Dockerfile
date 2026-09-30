FROM alpine

RUN apk add --no-cache gcc libc-dev linux-headers make

WORKDIR /source
COPY lib/ lib/
COPY *.c *.h Makefile ./

RUN make

FROM alpine

COPY --from=0 /source/jitterentropy-rngd /usr/bin

ENTRYPOINT ["/usr/bin/jitterentropy-rngd"]
CMD ["-v"]
