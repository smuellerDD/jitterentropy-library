# Module Signing Test Key

`signing_key.pem` holds an RSA-4096 private key and its self-signed X.509
certificate, both in one PEM file as the kernel's `CONFIG_MODULE_SIG_KEY`
expects. The small FIPS-mode kernel of the `raw-entropy-fips-vm` check in
`nix/kernel.nix` is built with it: the kernel signs its own modules with it
(`CONFIG_MODULE_SIG_ALL`), builds its certificate into the trusted keyring and
accepts signed modules only (`CONFIG_MODULE_SIG_FORCE`), and the out-of-tree
`jitter_rng.ko` built for that kernel is signed with it by `scripts/sign-file`.

The private key is public by being checked in. It exists so that the test
kernel and the module can be built reproducibly and signed without generating
a key per build. It is NOT FOR PRODUCTION: never trust its certificate on a
real system, and never sign anything with it that leaves a test environment.

It was generated with

```
openssl req -new -nodes -utf8 -sha256 -days 36500 -batch -x509 \
	-config x509.genkey -outform PEM -out signing_key.pem \
	-keyout signing_key.pem
```

following the kernel's `Documentation/admin-guide/module-signing.rst`.
