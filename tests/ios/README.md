# iOS example app

A small SwiftUI app that builds the Jitter RNG into an iOS app the way an app
would embed it: `CMakeLists.txt` includes the library's own `CMakeLists.txt`
and links the static library into the app, and `jitterentropy-bridging.h`
makes its C API visible to Swift. The app allocates one collector at start-up.
On the *Output* tab one button shows its `jent_status()` document, the other 32
bytes of its output in hex. Both are also written to the system log. On the
*Collector* tab three buttons replace the collector with a new one - without flags, in FIPS (`JENT_FORCE_FIPS`) or in
NTG.1 (`JENT_NTG1`) mode - after running the power-on tests with the same
flags. Both modes require the collector's state to be locked and fail with
EMEM where it cannot be; the memory access region is never locked.

The *Timer thread* toggle above those buttons allocates the next collector with
`JENT_FORCE_INTERNAL_TIMER`: its time stamps then come from the library's timer
thread, a thread counting in a loop, rather than from the platform clock. The
status document says which one a collector uses (`internalTimer`). The choice
is per collector: one allocated with the toggle off uses the platform clock
again. NTG.1 forbids the timer thread and is unavailable while the toggle is on.

The steppers below the toggle set the next collector's initial oversampling
rate (3 to 20), memory size (automatic, or `JENT_MAX_MEMSIZE_1kB` to
`JENT_MAX_MEMSIZE_512MB`) and hash loop count (default, or `JENT_HASHLOOP_1`
to `JENT_HASHLOOP_128`). A health test failure raises the oversampling rate and
the hash loop count of the replacement collector, but not the memory size: the
increment in `jent_update_memsize()` applies only to a size the library derived
itself, so a memory size chosen here is the one the instance keeps. Leave it on
automatic to let the recovery grow it. The status document reports what a
collector runs with.

The *Record* tab records the raw noise data of the SP800-90B and NTG.1
entropy assessment in the app, in the default, FIPS or NTG.1 mode and with the
settings of the *Collector* tab, as the recording scripts of
`tests/raw-entropy/recording_userspace` do on a host, into
`Documents/results-measurements`, which the Files app and the Finder show.
`tests/raw-entropy/recording_library/README.md` describes what it records, how to
copy it off the phone and how to analyze it.

- `Collector.swift` - one collector and the flags it is allocated with; every
  call into the library runs on its serial queue
- `Recorder.swift` - the recording of the *Record* tab, through
  `jent_record_raw()` of `libjitterentropy-record`, which `CMakeLists.txt`
  builds with `ENABLE_RECORDING` and links beside `libjitterentropy`
- `JitterEntropyExampleApp.swift` - the UI

## Building

Xcode and CMake on macOS. `xcodebuild`, `simctl` and `devicectl` come from
the active developer directory, which has to be Xcode's rather than the
Command Line Tools': `sudo xcode-select -s /Applications/Xcode.app`, or
`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` in the environment.
The Xcode generator writes an `.xcodeproj` that can also be opened in Xcode:

```
cmake -S tests/ios -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS
cmake --build build-ios --config Debug -- -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO
```

To run it in a booted simulator:

```
xcrun simctl install booted build-ios/Debug-iphonesimulator/JitterEntropyExample.app
xcrun simctl launch --console booted de.chronox.jitterentropy.example
```

## Running on a device

A device build is signed, so it needs a development team: an Apple ID added
in Xcode's Settings > Accounts, where a free personal team will do. Once Xcode
has loaded the account, `defaults read com.apple.dt.Xcode
IDEProvisioningTeamByIdentifier` lists its team IDs. The team is passed at
configure time, as the generator would overwrite a team picked in the
generated project, and `-allowProvisioningUpdates` lets `xcodebuild` create
the signing certificate and the provisioning profile. An App ID belongs to the
first team that registers it, so another team needs its own bundle identifier
in place of the default `de.chronox.jitterentropy.example`, set with
`-DBUNDLE_ID`:

```
cmake -S tests/ios -B build-ios-device -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=<team id> -DBUNDLE_ID=<bundle id>
cmake --build build-ios-device --config Debug -- -sdk iphoneos \
    -allowProvisioningUpdates -allowProvisioningDeviceRegistration
```

The first build of a new profile can stop with "unable to read input file
... .mobileprovision"; a second run picks the profile up.

The phone needs Developer Mode, under Settings > Privacy & Security, which
takes a restart. `xcrun devicectl list devices` shows it with its name and
identifier, either of which `--device` takes:

```
xcrun devicectl device install app --device <device> \
    build-ios-device/Debug-iphoneos/JitterEntropyExample.app
xcrun devicectl device process launch --console --terminate-existing \
    --device <device> <bundle id>
```

`--console` shows the app's log, whether the collector was allocated and the
output of each button, until the app exits or `devicectl` is stopped. An app
signed by a personal team only launches once its developer is trusted on the
phone, under Settings > General > VPN & Device Management; until then the
launch fails with "profile has not been explicitly trusted by the user".
