{
  description = "Archivist for NixOS and Nix-based desktops";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/1c89083e451e891152365b799595cf7790a43325";
  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          package = pkgs.callPackage ./packaging/nix/package.nix { source = self; };
        in { archivist = package; default = package; });
      checks = forAllSystems (system: {
        archivist = self.packages.${system}.archivist;
      });
    };
}
