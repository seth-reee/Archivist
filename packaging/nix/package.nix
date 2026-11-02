{ lib, stdenv, cmake, ninja, qt6, source, pkg-config, libarchive }:

stdenv.mkDerivation {
  pname = "archivist";
  version = builtins.elemAt (builtins.match ".*project\\(Archivist VERSION ([^ ]+) LANGUAGES.*"
    (builtins.readFile ../../CMakeLists.txt)) 0;
  src = lib.cleanSourceWith {
    src = source;
    filter = path: type:
      let name = baseNameOf path; in
      !(lib.hasInfix ".sync-conflict-" name)
      && !(builtins.elem name [ ".git" "build" "dist" "stage" "result" ])
      && !(lib.hasSuffix ".pkg.tar.zst" name)
      && !(lib.hasSuffix ".pkg.tar.xz" name);
  };
  nativeBuildInputs = [ cmake ninja qt6.wrapQtAppsHook pkg-config ];
  buildInputs = [ qt6.qtbase qt6.qtwayland libarchive ];
  cmakeFlags = [ "-DBUILD_TESTING=ON" ];
  doCheck = true;
  preCheck = ''
    export HOME="$TMPDIR/home"
    mkdir -p "$HOME"
    export TMPDIR=/tmp
    export QT_QPA_PLATFORM=offscreen
    export QT_QPA_PLATFORMTHEME=
    export QT_QUICK_CONTROLS_STYLE=Basic
    export QT_QUICK_BACKEND=software
  '';


  meta = {
    description = "Qt archive manager";
    homepage = "https://github.com/seth-reee/Archivist";
    license = lib.licenses.mit;
    mainProgram = "archivist";
    platforms = [ "x86_64-linux" "aarch64-linux" ];
  };
}
