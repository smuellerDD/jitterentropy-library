# The version and the systems the flake covers.
ctx:
let
  inherit (ctx) lib systems;
in
{
  # Read from jitterentropy.h, so it cannot drift from the header.
  jentVersion =
    let
      header = builtins.readFile ../jitterentropy.h;
      field = name: builtins.head (builtins.match
        ".*#define ${name} ([0-9]+)\n.*" header);
    in "${field "JENT_MAJVERSION"}.${field "JENT_MINVERSION"}.${field "JENT_PATCHLEVEL"}";

  systems = [ "x86_64-linux" "aarch64-linux" ];

  forAllSystems = f: lib.genAttrs systems (system: f system);
}
