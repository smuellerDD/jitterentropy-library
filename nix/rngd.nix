# jitterentropy-rngd as the systemd service it installs, in a NixOS VM.
ctx:
let
  inherit (ctx) toolsFor;
in
{
  # The daemon under the unit it ships: the unit's sandboxing has to leave it
  # what it needs, and it is active only once the kernel took an injection.
  rngdVmFor = pkgs:
    let tools = toolsFor pkgs;
    in pkgs.testers.runNixOSTest {
      name = "jitterentropy-rngd";

      nodes.machine = {
        boot.kernelParams = [ "clocksource=tsc" "tsc=reliable" ];
        virtualisation.qemu.options = [ "-cpu" "host" ];
        # The internal timer's counting thread needs a second CPU.
        virtualisation.cores = 2;
        environment.systemPackages = [ tools ];
        # jitterentropy.service as installed; NixOS ignores its [Install].
        systemd.packages = [ tools ];
        systemd.services.jitterentropy.wantedBy = [ "basic.target" ];
      };

      testScript = builtins.readFile ./rngd/test.py;
    };
}
