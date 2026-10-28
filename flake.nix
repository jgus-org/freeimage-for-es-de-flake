{
  description = "Minimal Classic-FreeImage-3-compatible shim library for ES-DE";

  inputs = {
    flake-lib.url = "github:jgus-org/flake-lib/v1";
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs =
    {
      self,
      flake-lib,
      nixpkgs,
      ...
    }:
    flake-lib.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        inherit (pkgs) lib;

        orgFormatting = flake-lib.lib.mkFormatting {
          inherit self system;
        };

        freeimage-for-es-de = pkgs.stdenv.mkDerivation (finalAttrs: {
          # Codec backends resolved through nixpkgs (verified on nixos-26.05).
          buildInputs = [
            pkgs.zlib
            pkgs.libjpeg_turbo
            pkgs.libpng
            pkgs.giflib
            pkgs.libwebp
          ];
          meta = {
            description = "Minimal Classic-FreeImage-3-compatible shim library for ES-DE";
            license = lib.licenses.mit;
          };
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.pkg-config
          ];
          pname = "freeimage-for-es-de";
          postInstall = ''
            install -Dm644 ${./freeimage.pc.in} $out/lib/pkgconfig/freeimage.pc
            substituteInPlace $out/lib/pkgconfig/freeimage.pc \
              --replace-fail '@out@' "$out" \
              --replace-fail '@version@' "${finalAttrs.version}"
            install -Dm644 ${./LICENSE.md} $out/share/licenses/freeimage-for-es-de/LICENSE
          '';
          src = ./.;
          version = "3.18.0";
        });

        # The doctest suite is opt-in in CMakeLists (FREEIMAGE_FOR_ES_DE_TESTS), so the
        # package build stays production-only and the suite runs here instead: the test
        # target compiles src/*.cpp itself and links the codec backends directly, so
        # it needs the library's own dependencies plus doctest. The `check` target runs
        # the binary, making the suite green a build-time requirement of this output.
        unit-tests = pkgs.stdenv.mkDerivation {
          inherit (freeimage-for-es-de) buildInputs src;
          # The shared library is not what the suite exercises, so build only the test
          # binary; the check target depends on it and runs it in place.
          buildFlags = [ "freeimage-for-es-de-tests" ];
          checkTarget = "check";
          cmakeFlags = [ "-DFREEIMAGE_FOR_ES_DE_TESTS=ON" ];
          doCheck = true;
          # The cmake hook configures in a `build` subdirectory; the install phase's
          # working directory is the hook's, not necessarily the source root.
          installPhase = ''
            runHook preInstall
            mkdir -p $out/bin
            install -m755 "$(find . -type f -name freeimage-for-es-de-tests)" $out/bin/
            runHook postInstall
          '';
          nativeBuildInputs = freeimage-for-es-de.nativeBuildInputs ++ [ pkgs.doctest ];
          pname = "freeimage-for-es-de-tests";
          version = freeimage-for-es-de.version;
        };

        check =
          name: source:
          pkgs.runCommand "freeimage-for-es-de-${name}"
            {
              nativeBuildInputs = [
                pkgs.pkg-config
                pkgs.stdenv.cc
                freeimage-for-es-de
              ];
            }
            ''
              c++ -std=c++23 ${source} $(pkg-config --cflags --libs freeimage) -o ${name}
              ./${name}
              mkdir -p $out
            '';

      in
      {
        checks = {
          esde-surface = check "esde_surface" ./tests/esde_surface.cpp;
          formatting = orgFormatting.check;
          link-all = check "link_all" ./tests/link_all.cpp;
          smoke = check "smoke" ./tests/smoke.cpp;
          unit = unit-tests;
        };
        formatter = orgFormatting.formatter;
        packages = {
          inherit freeimage-for-es-de;
          default = freeimage-for-es-de;
        };
      }
    );
}
