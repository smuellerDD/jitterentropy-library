# The systems the flake covers.
ctx:
let
  inherit (ctx) forAllSystems lib systems;
in
{
  # QEMU on the host architecture.
  systems = [ "x86_64-linux" "aarch64-linux" ];

  forAllSystems = f: lib.genAttrs systems (system: f system);
}
