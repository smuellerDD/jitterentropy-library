# Android: the library through the NDK, the example app and its emulator.
ctx:
let
  inherit (ctx) jentVersion lib nixpkgs pkgsAndroidFor self;
in
{
  # The Android SDK and NDK are unfree, hence a nixpkgs instance of their own.
  pkgsAndroidFor = system: import nixpkgs {
    inherit system;
    config = {
      allowUnfree = true;
      android_sdk.accept_license = true;
    };
  };

  # ndk-build over tests/android/Android.mk. APP_PLATFORM is the NDK's floor,
  # API 21, which keeps the /dev/urandom fallback for getrandom() (API 28)
  # compiled.
  androidFor = system:
    let
      pkgsAndroid = pkgsAndroidFor system;
      ndk = (pkgsAndroid.androidenv.composeAndroidPackages {
        includeNDK = true;
      }).ndk-bundle;
    in pkgsAndroid.stdenv.mkDerivation {
      pname = "jitterentropy-android";
      version = jentVersion;
      src = self;
      # cmake only runs the export check below.
      nativeBuildInputs = [ ndk pkgsAndroid.cmake ];
      dontConfigure = true;

      buildPhase = ''
        runHook preBuild
        ndk-build \
          NDK_PROJECT_PATH=null \
          APP_BUILD_SCRIPT=$(pwd)/tests/android/Android.mk \
          APP_PLATFORM=android-21 \
          APP_ABI="arm64-v8a x86_64" \
          APP_OPTIM=release \
          NDK_OUT=$TMPDIR/obj \
          NDK_LIBS_OUT=$TMPDIR/libs \
          -j"$NIX_BUILD_CORES" V=1
        runHook postBuild
      '';

      # The export check of the CMake suite, per ABI. It skips an nm output
      # it cannot parse, which the grep turns into a failure.
      doCheck = true;
      checkPhase = ''
        runHook preCheck
        for so in $TMPDIR/libs/*/libjitterentropy.so; do
          cmake -DJENT_LIB=$so \
            -DJENT_VERSION_SCRIPT=$(pwd)/version.lds \
            -DJENT_NM=$(command -v nm) "-DJENT_NM_ARGS=-D;-g" \
            -P cmake/JentCheckExports.cmake 2>&1 | tee $TMPDIR/exports.log
          grep -q "functions of the API" $TMPDIR/exports.log
        done
        runHook postCheck
      '';

      installPhase = ''
        runHook preInstall
        mkdir -p $out/lib
        cp -r $TMPDIR/libs/* $out/lib/
        runHook postInstall
      '';

      meta = {
        description =
          "Jitter RNG userspace library built with the Android NDK";
        license = lib.licenses.bsd3;
      };
    };

  # The example app in tests/android, built by Gradle.
  #
  # tests/android/deps.json lets mitmCache serve Gradle's Maven downloads
  # offline. After changing anything Gradle downloads, regenerate it:
  #
  #   $(nix build --no-link --print-out-paths \
  #       .#android-example.mitmCache.updateScript)
  #
  # The SDK must hold exactly what app/build.gradle.kts pins: AGP cannot
  # install into the read-only store.
  androidAppFor = system:
    let
      pkgsAndroid = pkgsAndroidFor system;
      buildTools = "37.0.0";
      sdk = (pkgsAndroid.androidenv.composeAndroidPackages {
        platformVersions = [ "37.0" ];
        buildToolsVersions = [ buildTools ];
        cmakeVersions = [ "4.1.2" ];
        includeNDK = true;
        ndkVersions = [ "29.0.14206865" ];
      }).androidsdk;
      androidHome = "${sdk}/libexec/android-sdk";
      gradle = pkgsAndroid.gradle_9;
    in pkgsAndroid.stdenv.mkDerivation (finalAttrs: {
      pname = "jitterentropy-android-example";
      version = jentVersion;
      src = self;

      nativeBuildInputs = [ gradle ];

      mitmCache = gradle.fetchDeps {
        pkg = finalAttrs.finalPackage;
        data = ../tests/android/deps.json;
        # ninja needs /bin/sh, which the update script's sandbox lacks.
        bwrapFlags = ''--ro-bind "$PWD" "$PWD" --ro-bind /bin /bin'';
      };

      env.ANDROID_HOME = androidHome;

      # The aapt2 AGP fetches from Maven does not run on NixOS; the SDK's
      # copy is patched.
      gradleFlags = [
        "-Dorg.gradle.project.android.aapt2FromMavenOverride=${androidHome}/build-tools/${buildTools}/aapt2"
      ];
      gradleBuildTask = "assembleDebug";
      # The default, nixDownloadDeps, also resolves the androidTest
      # classpaths, which an application module cannot resolve.
      gradleUpdateTask = finalAttrs.gradleBuildTask;

      # AGP writes its debug keystore below ANDROID_USER_HOME, which defaults
      # to the unwritable $HOME.
      postPatch = ''
        cd tests/android
        export ANDROID_USER_HOME=$(mktemp -d)
      '';

      installPhase = ''
        runHook preInstall
        install -Dm644 app/build/outputs/apk/debug/app-debug.apk \
          $out/jitterentropy-example.apk
        runHook postInstall
      '';

      meta = {
        description = "Jitter RNG example app for Android";
        license = lib.licenses.bsd3;
        sourceProvenance = with lib.sourceTypes; [
          fromSource
          binaryBytecode # the Gradle plugin from mitmCache
        ];
      };
    });

  # `nix run .#android-example-emulator` boots an x86_64 emulator, installs
  # and starts the app. Needs /dev/kvm, and a display unless
  # NIX_ANDROID_EMULATOR_FLAGS=-no-window.
  androidEmulatorFor = system: app:
    (pkgsAndroidFor system).androidenv.emulateApp {
      name = "jitterentropy-android-example-emulator";
      inherit app;
      platformVersion = "36";
      abiVersion = "x86_64";
      systemImageType = "default";
      package = "de.chronox.jitterentropy.example";
      activity = ".MainActivity";
    };
}
