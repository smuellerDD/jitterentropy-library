# The CMake build of the library and its tools: native, musl, static and
# cross-compiled.
ctx:
let
  inherit (ctx) crossFor jentVersion lib nixpkgs self toolsFor;

  # The noise source has to be the code make and cmake build, so the wrapper
  # hardening that changes it or cannot apply is off:
  # - fortify, fortify3: cannot combine with the -O0 of the entropy core
  # - zerocallusedregs, stackclashprotection: add instructions to the timed
  #   loop that neither build system emits
  # - strictflexarrays1: a flag neither build passes
  hardeningDisable = [ "fortify" "fortify3" "zerocallusedregs"
                       "stackclashprotection" "strictflexarrays1" ];
in
{
  # The library plus its tools in bin.
  toolsFor = pkgs:
    pkgs.stdenv.mkDerivation {
      pname = "jitterentropy-tools";
      version = jentVersion;
      src = self;
      nativeBuildInputs = [ pkgs.cmake ];
      # The VM tests run jitter_rng against the kernel module, and the daemon.
      cmakeFlags = [ "-DENABLE_KERNEL_LIB=ON" "-DENABLE_RNGD=ON" ];
      enableParallelBuilding = true;
      inherit hardeningDisable;
      meta = {
        description = "Jitter RNG userspace library and validation tools";
        license = lib.licenses.bsd3;
      };
    };

  # musl differs from glibc in CPU_SET pinning, mlock/getrlimit and
  # clock_gettime(). pkgsMusl, not pkgsCross.musl64, so the tools run on the
  # build machine. CI builds only muslStaticFor.
  muslFor = pkgs:
    (toolsFor pkgs.pkgsMusl).overrideAttrs
    (_: { pname = "jitterentropy-tools-musl"; });

  # Static: pthreads and clock_gettime() without a dynamic loader. The static
  # stdenv names itself in the store path, hence no pname.
  muslStaticFor = pkgs: toolsFor pkgs.pkgsStatic;

  # Compile-and-link smoke builds for targets whose arch/ backends nothing
  # else here reaches. Not run.
  crossFor = { cross, timer ? true, shared ? false }:
    cross.stdenv.mkDerivation {
      pname = "jitterentropy-cross";
      version = jentVersion;
      src = self;
      nativeBuildInputs = [ nixpkgs.legacyPackages.x86_64-linux.cmake ];
      # The nixpkgs cmake hook passes BUILD_TESTING=OFF, and the tests are the
      # code that reaches these targets' arch backends.
      cmakeFlags = [ "-DINTERNAL_TIMER=${if timer then "on" else "off"}"
                     "-DBUILD_TESTING=ON" ]
        ++ lib.optional shared "-DBUILD_SHARED_LIBS=ON";
      enableParallelBuilding = true;
      inherit hardeningDisable;
      meta = {
        description = "Jitter RNG cross-compilation smoke build";
        license = lib.licenses.bsd3;
      };
    };

  crossTargets = pkgs:
    let p = pkgs.pkgsCross;
    in {
      # The dedicated jent_get_nstime() backends; armv7 covers the
      # clock_gettime() fallback and the 32-bit paths.
      cross-s390x = crossFor { cross = p.s390x; };
      cross-ppc64 = crossFor { cross = p.powernv; };
      cross-riscv64 = crossFor { cross = p.riscv64; };
      cross-loongarch64 = crossFor { cross = p.loongarch64-linux; };
      cross-armv7 = crossFor { cross = p."armv7l-hf-multiplatform"; };

      # Both link widths, plus shared for the dllexport/dllimport split and
      # the bcrypt import library. The internal timer needs no <pthread.h>
      # here, only the CRT and kernel32.
      cross-mingwW64 = crossFor { cross = p.mingwW64; };
      cross-mingwW64-shared = crossFor { cross = p.mingwW64; shared = true; };
      cross-mingw32 = crossFor { cross = p.mingw32; };
    };
}
