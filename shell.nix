{ pkgs ? import <nixpkgs> {}, cross ? false }:

if cross then (
  # =========================================================================
  # Windows Cross-Compilation Shell (via pkgsCross.mingwW64)
  # =========================================================================
  let
    mingwPkgs = pkgs.pkgsCross.mingwW64;
  in
  mingwPkgs.mkShell {
    name = "desktopwebview-cross-shell";
    
    nativeBuildInputs = [
      mingwPkgs.buildPackages.cmake
      mingwPkgs.buildPackages.ninja
      mingwPkgs.buildPackages.gnumake
      mingwPkgs.buildPackages.pkg-config
      pkgs.wine64
    ];

    buildInputs = [
      mingwPkgs.openssl
      mingwPkgs.libxml2
      mingwPkgs.libiconv  # libxml2's static archive references iconv symbols
    ];

    shellHook = ''
      echo "=========================================================="
      echo "     DesktopWebview Windows Cross-Compilation Shell      "
      echo "=========================================================="
      echo "Available commands:"
      echo "  1. build-windows      : Cross-compile for Windows using MinGW"
      echo "  2. run-windows        : Run the Windows build using Wine"
      echo "  3. run-test-windows   : Run the Windows TestNet + TestWrapper builds using Wine"
      echo "=========================================================="

      build-windows() {
        echo "Configuring CMake for Windows cross-compilation (MinGW)..."
        rm -rf build-windows
        cmake -B build-windows -S . \
          -DCMAKE_SYSTEM_NAME=Windows \
          -DCMAKE_BUILD_TYPE=Release

        echo "Building for Windows..."
        cmake --build build-windows -j$(nproc)
      }

      # Copy the runtime DLLs the executables link against into build-windows/lib/
      # for Wine: OpenSSL, the iconv libs pulled in by libxml2, and the
      # MinGW mcf threading runtime (libmcfgthread-2.dll) which stays dynamic
      # even with -static-libgcc/-static-libstdc++.
      copy-windows-dlls() {
        mkdir -p build-windows/lib
        echo "Copying runtime DLLs (OpenSSL + iconv + mcfgthread) to build-windows/lib/..."
        cp -f ${mingwPkgs.openssl}/bin/*.dll build-windows/lib/
        cp -f ${mingwPkgs.libiconv}/bin/*.dll build-windows/lib/
        cp -f ${mingwPkgs.windows.mcfgthreads}/bin/*.dll build-windows/lib/
      }

      run-windows() {
        if [ -f build-windows/DesktopWebview.exe ]; then
          copy-windows-dlls
          echo "Running Windows executable via Wine..."
          WINEPATH="$(pwd)/build-windows/lib;${WINEPATH:+$WINEPATH}" wine64 build-windows/DesktopWebview.exe
        else
          echo "Error: build-windows/DesktopWebview.exe not found. Run build-windows first."
        fi
      }

      run-test-windows() {
        if [ ! -f build-windows/TestNet.exe ] || [ ! -f build-windows/TestWrapper.exe ]; then
          echo "Error: test executables not found in build-windows/. Run build-windows first."
          return 1
        fi
        copy-windows-dlls
        local status=0
        export WINEPATH="$(pwd)/build-windows/lib;${WINEPATH:+$WINEPATH}"
        echo "=========================================================="
        echo "Running Windows TestNet executable via Wine..."
        echo "=========================================================="
        wine64 build-windows/TestNet.exe || status=$?
        echo "=========================================================="
        echo "Running Windows TestWrapper executable via Wine..."
        echo "=========================================================="
        wine64 build-windows/TestWrapper.exe || status=$?
        return $status
      }
    '';
  }
) else (
  # =========================================================================
  # Native Linux Development Shell
  # =========================================================================
  pkgs.mkShell {
    name = "desktopwebview-native-shell";

    nativeBuildInputs = [
      pkgs.cmake
      pkgs.pkg-config
      pkgs.ninja
    ];

    buildInputs = [
      pkgs.xorg.libX11
      pkgs.xorg.libX11.dev  # Required to find headers like X11/Xlib.h
      pkgs.xorg.libxcb
      pkgs.xorg.libxcb.dev
      pkgs.xorg.libXau
      pkgs.xorg.libXau.dev
      pkgs.xorg.libXdmcp
      pkgs.xorg.libXdmcp.dev
      pkgs.openssl       # Provides the OpenSSL libraries and headers
      pkgs.openssl.dev
      pkgs.libxml2
    ];

    shellHook = ''
      ln -sf build-linux/compile_commands.json compile_commands.json
      export PKG_CONFIG_PATH="${pkgs.xorg.libX11.dev}/lib/pkgconfig:${pkgs.xorg.libxcb.dev}/lib/pkgconfig:${pkgs.xorg.libXau.dev}/lib/pkgconfig:${pkgs.xorg.libXdmcp.dev}/lib/pkgconfig:$PKG_CONFIG_PATH"

      echo "=========================================================="
      echo "         DesktopWebview Native Linux Development Shell    "
      echo "=========================================================="
      echo "Available commands:"
      echo "  1. build-linux   : Configure and build natively for Linux"
      echo "  2. run-linux     : Run the native Linux build"
      echo "=========================================================="

      build-linux() {
        echo "Configuring CMake for Native Linux..."
        cmake -B build-linux -S . \
          -DCMAKE_BUILD_TYPE=Debug
        ln -sf build-linux/compile_commands.json compile_commands.json
        echo "Building..."
        cmake --build build-linux -j$(nproc)
      }

      run-linux() {
        if [ -f build-linux/DesktopWebview ]; then
          ./build-linux/DesktopWebview
        else
          echo "Error: build-linux/DesktopWebview not found. Run build-linux first."
        fi
      }
    '';
  }
)
