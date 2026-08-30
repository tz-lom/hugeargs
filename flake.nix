{
  description = "Debug build of binutils";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        binutilsDebug = pkgs.binutils.overrideAttrs (oldAttrs: {
          dontStrip = true;
          separateDebugInfo = false;
          configureFlags = (oldAttrs.configureFlags or []) ++ [
            "--enable-debug"
          ];
          CFLAGS = "-g -O0";
          CXXFLAGS = "-g -O0";
        });
      in
      {
        packages.default = binutilsDebug;

        devShells.default = pkgs.mkShell {
          buildInputs = [
            binutilsDebug
            pkgs.gdb
            pkgs.gcc
            pkgs.gnumake
          ];
        };
      }
    );
}
