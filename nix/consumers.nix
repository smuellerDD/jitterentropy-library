# The crypto libraries: the library built against them, and the libraries and
# tools that use the Jitter RNG built against this tree.
ctx:
let
  inherit (ctx) jentVersion lib self toolsFor;
in
{
  # The consumers built against this tree, which is where a header, export or
  # link dependency that broke them shows.
  #
  # nixpkgs' own derivation with src and cmakeFlags replaced, so the library
  # is tested as a distribution installs it. rng-tools and ESDM compile and
  # link only; the crypto libraries also run what exercises their copy.
  consumersFor = pkgs:
    let
      # nixpkgs installs the static archive. Its symbols differ with the
      # timer: rng-tools' configure link-tests jent_notime_settick() and
      # compiles its notime thread only if found. Hence both: off is what
      # distributions build, on is this repository's default.
      #
      # ESDM gates much of esdm_es_jent.c on JENT_VERSION, which pins the
      # widest part of the API.
      libFor = timer:
        pkgs.jitterentropy.overrideAttrs (_: {
          version = jentVersion;
          src = self;
          cmakeFlags =
            [ "-DINTERNAL_TIMER=${if timer then "on" else "off"}" ];
        });
      notimer = libFor false;
      timer = libFor true;
      buildOnly = drv:
        drv.overrideAttrs (_: {
          doCheck = false;
          doInstallCheck = false;
        });
      # Newer ESDM releases neither export the library's symbols nor keep
      # them past the strip, so the build object is what shows it linked.
      esdmFor = jent:
        (buildOnly (pkgs.esdm.override { jitterentropy = jent; }))
          .overrideAttrs (old: {
            postBuild = (old.postBuild or "") + ''
              $NM --defined-only esdm/libesdm.so |
                grep -E ' [Tt] jent_entropy_init$'
            '';
          });
    in {
      consumer-rng-tools =
        buildOnly (pkgs.rng-tools.override { jitterentropy = notimer; });
      consumer-rng-tools-timer =
        buildOnly (pkgs.rng-tools.override { jitterentropy = timer; });
      consumer-esdm = esdmFor notimer;
      consumer-esdm-timer = esdmFor timer;

      # Botan's jitter_rng module, which runs the library with
      # JENT_FORCE_FIPS. Only its jitter_rng tests, not the whole suite; the
      # install check draws from it through the CLI.
      consumer-botan = (pkgs.botan3.override {
        withJitterentropy = true;
        jitterentropy = notimer;
      }).overrideAttrs (_: {
        checkPhase = ''
          runHook preCheck
          # The library is only in the build directory yet.
          LD_LIBRARY_PATH=$PWD ./botan-test jitter_rng
          runHook postCheck
        '';
        doInstallCheck = true;
        installCheckPhase = ''
          runHook preInstallCheck
          $bin/bin/botan rng --jitter 32
          runHook postInstallCheck
        '';
      });

      # OpenSSL's JITTER seed source (enable-jitter), which links
      # libjitterentropy.a. Its test suite never selects JITTER; the install
      # check does, and fails if seeding from it fails.
      consumer-openssl = pkgs.openssl.overrideAttrs (old: {
        configureFlags = old.configureFlags ++ [
          "enable-jitter"
          "--with-jitter-include=${lib.getDev notimer}/include"
          "--with-jitter-lib=${lib.getLib notimer}/lib"
        ];
        doCheck = false;
        doInstallCheck = true;
        installCheckPhase = ''
          runHook preInstallCheck
          openssl=$bin/bin/openssl
          $openssl list -random-generators | grep -w JITTER
          OPENSSL_CONF=test/default-and-jitter.cnf $openssl rand -hex 32
          runHook postInstallCheck
        '';
      });

      # AWS-LC and libgcrypt compile a vendored copy of the sources. This
      # tree takes the copy's place, and each library's own test suite runs.

      # The CMakeLists.txt names the sources of the layout it vendored; the
      # list is replaced with every source here.
      consumer-awslc = pkgs.aws-lc.overrideAttrs (old: {
        postPatch = (old.postPatch or "") + ''
          jent=third_party/jitterentropy/jitterentropy-library
          rm -rf $jent
          mkdir $jent
          cp -r --no-preserve=mode ${self}/jitterentropy.h ${self}/src \
            ${self}/arch $jent/

          sources=$(cd $jent && ls src/*.c arch/*.c |
            sed 's|^|        ''${PROJECT_SOURCE_DIR}/'"$jent"'/|')
          awk -v sources="$sources" '
            /^set\(JITTER_SOURCES$/ { print; print sources; skip = 1; next }
            skip && /\)$/ { print ")"; skip = 0; next }
            !skip' \
            third_party/jitterentropy/CMakeLists.txt > CMakeLists.jent
          mv CMakeLists.jent third_party/jitterentropy/CMakeLists.txt
          substituteInPlace third_party/jitterentropy/CMakeLists.txt \
            --replace-fail \
              '"''${PROJECT_SOURCE_DIR}/'"$jent"'")' \
              '"''${PROJECT_SOURCE_DIR}/'"$jent"'" "''${PROJECT_SOURCE_DIR}/'"$jent"'/src" "''${PROJECT_SOURCE_DIR}/'"$jent"'/arch")'
          grep -A30 '^set(JITTER_SOURCES$' \
            third_party/jitterentropy/CMakeLists.txt

          # The unit test reads the oversampling rate out of struct
          # rand_data, opaque since 3.7, and pins the vendored version.
          substituteInPlace crypto/fipsmodule/rand/cpu_jitter_test.cc \
            --replace-fail \
              'jitter_ec.instance->osr' \
              'jent_entropy_collector_osr(jitter_ec.instance)' \
            --replace-fail \
              'unsigned int jitter_version = 3060300;' \
              'unsigned int jitter_version = JENT_VERSION;'
        '';
      });

      # rndjent.c #includes the sources as one private translation unit, so
      # the include list is the recording library's. LIBGCRYPT selects the
      # library's libgcrypt backend, whose public gcry_ functions are mapped
      # to the internal ones. tests/random reports whether the Jitter RNG ran.
      consumer-libgcrypt = pkgs.libgcrypt.overrideAttrs (old: {
        postPatch = (old.postPatch or "") + ''
          cp --no-preserve=mode ${self}/jitterentropy.h ${self}/src/* \
            ${self}/arch/* random/
          ln -s . random/arch

          {
            cat <<'EOF'
          #define LIBGCRYPT
          #undef gcry_malloc
          #undef gcry_malloc_secure
          #undef gcry_free
          #undef gcry_is_secure
          #undef gcry_fips_mode_active
          #define gcry_malloc _gcry_malloc
          #define gcry_malloc_secure _gcry_malloc_secure
          #define gcry_free _gcry_free
          #define gcry_is_secure _gcry_is_secure
          #define gcry_fips_mode_active() fips_mode ()
          #define JENT_GCRY_STARTED() 1
          #define JENT_GCRY_INITIALIZED() 1
          EOF
            sed -n 's|^#include "\.\./\.\./\.\./[a-z]*/\(.*\)"$|#include "\1"|p' \
              ${self}/tests/raw-entropy/recording_library/jitterentropy-record-lib.c
          } > random/jitterentropy-consumer.c
          cat random/jitterentropy-consumer.c

          sed -i random/rndjent.c -e '
            /^#include "jitterentropy-base.c"$/,/^#include "jitterentropy-timer.c"$/c\
          #include "jitterentropy-consumer.c"'
          grep -q '^#include "jitterentropy-consumer.c"$' random/rndjent.c
        '';
        postCheck = (old.postCheck or "") + ''
          tests/random --verbose 2>&1 | tee random.log
          grep -q 'The JENT RNG was active' random.log
        '';
      });

      # SymCrypt builds the library as a submodule with its Makefile and
      # links libjitterentropy.a into its FIPS module as the source of the
      # module's DRBG. Not in nixpkgs, so built here.
      #
      # The module checks an HMAC over its own image at load, so Nix's strip
      # and patchelf must not run.
      #
      # A failed init, allocation or read is SymCryptFatal(). The install
      # check draws from the installed module, the check phase runs the unit
      # tests against it.
      consumer-symcrypt = pkgs.stdenv.mkDerivation (finalAttrs: {
        pname = "symcrypt";
        version = "103.13.0";

        src = pkgs.fetchFromGitHub {
          owner = "microsoft";
          repo = "SymCrypt";
          tag = "v${finalAttrs.version}";
          hash = "sha256-4QQgeM2SgT6fLGNbp/OacBfP8svxVDCGg93yY06p3VM=";
        };

        postPatch = ''
          rm -rf 3rdparty/jitterentropy-library
          cp -r --no-preserve=mode ${self} 3rdparty/jitterentropy-library
        '';

        nativeBuildInputs = [
          pkgs.cmake
          (pkgs.python3.withPackages (ps: [ ps.pyelftools ]))
        ];

        # scripts/version.py asks git unless these are set.
        env = {
          SYMCRYPT_BRANCH = "v${finalAttrs.version}";
          SYMCRYPT_COMMIT_HASH = "0000000";
          SYMCRYPT_COMMIT_TIMESTAMP = "1980-01-01T00:00:00+00:00";
        };

        cmakeFlags = [ "-DSYMCRYPT_FIPS_BUILD=ON" ];

        dontStrip = true;
        dontPatchELF = true;

        doCheck = true;
        # The module keeps only the symbols of its integrity check; the copy
        # split off before the strip shows the library linked in.
        checkPhase = ''
          runHook preCheck
          $NM module/generic/.debug/libsymcrypt.so.${finalAttrs.version} |
            grep -E ' [Tt] jent_entropy_init$'
          exe/symcryptunittest noperftests \
            dynamic:$PWD/module/generic/libsymcrypt.so
          runHook postCheck
        '';

        installPhase = ''
          runHook preInstall
          mkdir -p $out/lib $out/include $out/bin
          cp -P module/generic/libsymcrypt.so* $out/lib/
          cp -r ../inc/. inc/. $out/include/
          cp exe/symcryptunittest $out/bin/
          runHook postInstall
        '';

        doInstallCheck = true;
        installCheckPhase = ''
          runHook preInstallCheck
          cat > random.c <<'EOF'
          #include <stdio.h>
          #include "symcrypt.h"

          int main(void)
          {
            BYTE buf[32];
            SIZE_T i;

            SymCryptModuleInit(SYMCRYPT_CODE_VERSION_API,
                               SYMCRYPT_CODE_VERSION_MINOR);
            SymCryptRandom(buf, sizeof(buf));
            for (i = 0; i < sizeof(buf); i++)
              printf("%02x", buf[i]);
            printf("\n");
            return 0;
          }
          EOF
          $CC -I$out/include random.c -o random -L$out/lib -lsymcrypt \
            -Wl,-rpath,$out/lib
          ./random
          runHook postInstallCheck
        '';

        meta = {
          description = "SymCrypt built against this tree";
          homepage = "https://github.com/microsoft/SymCrypt";
          license = lib.licenses.mit;
          platforms = [ "x86_64-linux" "aarch64-linux" ];
        };
      });
    };

  # The EXTERNAL_CRYPTO backends: the library calls the named library for
  # SHA-3 and secure memory, code the default build never compiles.
  #
  # BUILD_SHARED_LIBS because the nix-crypto CI job reads the NEEDED entries
  # of lib/libjitterentropy.so. BUILD_TESTING because only the unit suite
  # runs these backends deterministically.
  cryptoFor = pkgs:
    let
      backendFor = { name, external, dep }:
        (toolsFor pkgs).overrideAttrs (old: {
          pname = "jitterentropy-tools-${name}";
          buildInputs = (old.buildInputs or [ ]) ++ lib.toList dep;
          cmakeFlags = (old.cmakeFlags or [ ]) ++ [
            "-DEXTERNAL_CRYPTO=${external}"
            "-DBUILD_SHARED_LIBS=ON"
            "-DBUILD_TESTING=ON"
          ];
          doCheck = true;
          checkPhase = ''
            runHook preCheck
            ctest --output-on-failure -LE unreliable \
              -j "$NIX_BUILD_CORES"
            runHook postCheck
          '';
        });
    in {
      crypto-openssl = backendFor {
        name = "openssl";
        external = "OPENSSL";
        dep = pkgs.openssl;
      };
      crypto-awslc = backendFor {
        name = "awslc";
        external = "AWSLC";
        dep = pkgs.aws-lc;
      };
      # gcrypt.h includes <gpg-error.h>, which libgcrypt does not propagate.
      crypto-libgcrypt = backendFor {
        name = "libgcrypt";
        external = "LIBGCRYPT";
        dep = [ pkgs.libgcrypt pkgs.libgpg-error ];
      };
    };
}
