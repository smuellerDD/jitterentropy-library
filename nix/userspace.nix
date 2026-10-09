# The userspace library and tools: native, musl and cross builds.
ctx:
let
  inherit (ctx) crossFor crossTargets lib muslFor muslStaticFor nixpkgs self
    toolsFor;
in
{
  # The CMake build: the library plus its tools in bin.
  toolsFor = pkgs:
    pkgs.stdenv.mkDerivation {
      pname = "jitterentropy-tools";
      version = "3.8.0";
      src = self;
      nativeBuildInputs = [ pkgs.cmake ];
      enableParallelBuilding = true;
      # The entropy core must build at -O0, which _FORTIFY_SOURCE cannot
      # combine with: 17 "requires compiling with optimization" warnings,
      # one per translation unit. No hardening is given up - glibc defines
      # the _chk variants only under optimization, so the flag was already
      # inert, and the object file is byte-identical without it.
      hardeningDisable = [ "fortify" "fortify3" ];
      meta = {
        description = "Jitter RNG userspace library and validation tools";
        license = lib.licenses.bsd3;
      };
    };

  # The same tools against musl, which disagrees with glibc about much the
  # library uses: the CPU_SET pinning, the mlock/getrlimit handling, and
  # clock_gettime(), which it keeps in libc. pkgsMusl, not pkgsCross.musl64,
  # so the tools run on the machine that built them.
  #
  # CI covers dynamic musl through Debian's musl-gcc and builds only
  # muslStaticFor from here; this stays for use out of the flake directly.
  muslFor = pkgs:
    (toolsFor pkgs.pkgsMusl).overrideAttrs
    (_: { pname = "jitterentropy-tools-musl"; });

  # The same libc with the linkage changed: static is where the use of
  # pthreads and clock_gettime() has to hold up with no dynamic loader. The
  # static stdenv already names itself in the store path, hence no pname.
  muslStaticFor = pkgs: toolsFor pkgs.pkgsStatic;

  # Compile-and-link smoke builds for the targets nothing else here
  # evaluates - the ones whose arch/ backends are selected by preprocessor
  # conditions. Not run, which still catches the breakage that occurs: a
  # missing declaration, an absent header, inline asm that does not
  # assemble.
  crossFor = { cross, timer ? true, shared ? false }:
    cross.stdenv.mkDerivation {
      pname = "jitterentropy-cross";
      version = "3.8.0";
      src = self;
      nativeBuildInputs = [ nixpkgs.legacyPackages.x86_64-linux.cmake ];
      # BUILD_TESTING explicitly: the nixpkgs cmake hook passes it as OFF,
      # and here the test programs are a good part of what is worth
      # compiling - they are the code that reaches these targets' arch
      # backends directly.
      cmakeFlags = [ "-DINTERNAL_TIMER=${if timer then "on" else "off"}"
                     "-DBUILD_TESTING=ON" ]
        ++ lib.optional shared "-DBUILD_SHARED_LIBS=ON";
      enableParallelBuilding = true;
      # As in toolsFor, and it matters more here: these exist to surface
      # diagnostics, which 17 lines per target would bury.
      hardeningDisable = [ "fortify" "fortify3" ];
      meta = {
        description = "Jitter RNG cross-compilation smoke build";
        license = lib.licenses.bsd3;
      };
    };

  crossTargets = pkgs:
    let p = pkgs.pkgsCross;
    in {
      # The dedicated jent_get_nstime() backends: stcke, the PowerPC
      # timebase, rdtime, rdtime.d. armv7 covers the clock_gettime()
      # fallback and the 32-bit paths.
      cross-s390x = crossFor { cross = p.s390x; };
      cross-ppc64 = crossFor { cross = p.powernv; };
      cross-riscv64 = crossFor { cross = p.riscv64; };
      cross-loongarch64 = crossFor { cross = p.loongarch64-linux; };
      cross-armv7 = crossFor { cross = p."armv7l-hf-multiplatform"; };

      # Both link widths, plus shared - the only configuration exercising
      # the dllexport/dllimport split and the bcrypt import library. The
      # internal timer builds despite nixpkgs' mingw-w64 shipping no
      # <pthread.h>: the Win32 back-end needs only the CRT and kernel32,
      # which is what this target proves.
      cross-mingwW64 = crossFor { cross = p.mingwW64; };
      cross-mingwW64-shared = crossFor { cross = p.mingwW64; shared = true; };
      cross-mingw32 = crossFor { cross = p.mingw32; };
    };
}
