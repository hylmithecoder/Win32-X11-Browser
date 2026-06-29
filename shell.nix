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
      mingwPkgs.vulkan-loader
      mingwPkgs.ffmpeg  # libav* for Windows video decode (optional; guarded in CMake)
      pkgs.xorg.libX11
      pkgs.xorg.libXext
      pkgs.libGL
      pkgs.vulkan-headers
    ];

    shellHook = ''
      export LD_LIBRARY_PATH="/run/opengl-driver/lib:/run/opengl-driver-32/lib:${pkgs.xorg.libX11}/lib:${pkgs.xorg.libXext}/lib:${pkgs.libGL}/lib:$LD_LIBRARY_PATH"

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
        echo "Copying runtime DLLs (OpenSSL + iconv + mcfgthread + Vulkan) to build-windows/lib/..."
        cp -f ${mingwPkgs.openssl}/bin/*.dll build-windows/lib/
        cp -f ${mingwPkgs.libiconv}/bin/*.dll build-windows/lib/
        cp -f ${mingwPkgs.windows.mcfgthreads}/bin/*.dll build-windows/lib/
        cp -f ${mingwPkgs.vulkan-loader}/bin/*.dll build-windows/lib/
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
        if [ ! -f build-windows/TestNet.exe ] || [ ! -f build-windows/TestWrapper.exe ] || [ ! -f build-windows/TestCss.exe ] || [ ! -f build-windows/TestLayout.exe ]; then
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
        echo "=========================================================="
        echo "Running Windows TestCss executable via Wine..."
        echo "=========================================================="
        wine64 build-windows/TestCss.exe || status=$?
        echo "=========================================================="
        echo "Running Windows TestLayout executable via Wine..."
        echo "=========================================================="
        wine64 build-windows/TestLayout.exe || status=$?
        echo "=========================================================="
        echo "Running Windows TestPaint executable via Wine..."
        echo "=========================================================="
        wine64 build-windows/TestPaint.exe || status=$?
        echo "=========================================================="
        echo "Running Windows TestImage executable via Wine..."
        echo "=========================================================="
        wine64 build-windows/TestImage.exe || status=$?
        echo "=========================================================="
        echo "Running Windows TestSvg executable via Wine..."
        echo "=========================================================="
        wine64 build-windows/TestSvg.exe || status=$?
        echo "=========================================================="
        echo "Running Windows TestVideo executable via Wine..."
        echo "=========================================================="
        wine64 build-windows/TestVideo.exe || status=$?
        echo "=========================================================="
        echo "Running Windows TestAudio executable via Wine..."
        echo "=========================================================="
        wine64 build-windows/TestAudio.exe || status=$?
        echo "=========================================================="
        echo "Running Windows TestJs executable via Wine..."
        echo "=========================================================="
        wine64 build-windows/TestJs.exe || status=$?
        echo "=========================================================="
        echo "Running Windows TestBase64 executable via Wine..."
        echo "=========================================================="
        wine64 build-windows/TestBase64.exe || status=$?
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
      pkgs.vulkan-headers
      pkgs.vulkan-loader
      pkgs.vulkan-validation-layers
      pkgs.alsa-lib       # ALSA audio output backend (libasound)
      pkgs.alsa-lib.dev
      pkgs.ffmpeg         # libavformat/libavcodec/libavutil/libswscale (video decode)
      pkgs.ffmpeg.dev
      pkgs.opencl-headers # OpenCL C headers (CL/cl.h)
      pkgs.ocl-icd        # OpenCL ICD loader (libOpenCL.so)
    ];

    shellHook = ''
      ln -sf build-linux/compile_commands.json compile_commands.json
      export PKG_CONFIG_PATH="${pkgs.xorg.libX11.dev}/lib/pkgconfig:${pkgs.xorg.libxcb.dev}/lib/pkgconfig:${pkgs.xorg.libXau.dev}/lib/pkgconfig:${pkgs.xorg.libXdmcp.dev}/lib/pkgconfig:$PKG_CONFIG_PATH"
      export VK_LAYER_PATH="${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d"
      export LD_LIBRARY_PATH="${pkgs.vulkan-loader}/lib:${pkgs.lib.makeLibraryPath [ pkgs.libGL ]}:$LD_LIBRARY_PATH"

      echo "=========================================================="
      echo "         DesktopWebview Native Linux Development Shell    "
      echo "=========================================================="
      echo "Available commands:"
      echo "  1. build-linux   : Configure and build natively for Linux"
      echo "  2. run-linux     : Run the native Linux build"
      echo "  3. run-test-linux: Run the test executables natively for Linux"
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

      run-test-linux() {
        if [ ! -f build-linux/TestNet ] || [ ! -f build-linux/TestWrapper ] || [ ! -f build-linux/TestCss ]; then
          echo "Error: test executables not found in build-linux/. Run build-linux first."
          return 1
        fi
        local status=0
        echo "=========================================================="
        echo "Running Linux TestNet executable..."
        echo "=========================================================="
        ./build-linux/TestNet || status=$?
        echo "=========================================================="
        echo "Running Linux TestWrapper executable..."
        echo "=========================================================="
        ./build-linux/TestWrapper || status=$?
        echo "=========================================================="
        echo "Running Linux TestCss executable..."
        echo "=========================================================="
        ./build-linux/TestCss || status=$?
        echo "=========================================================="
        echo "Running Linux TestLayout executable..."
        echo "=========================================================="
        ./build-linux/TestLayout || status=$?
        echo "=========================================================="
        echo "Running Linux TestPaint executable..."
        echo "=========================================================="
        ./build-linux/TestPaint || status=$?
        echo "=========================================================="
        echo "Running Linux TestWindowPaint executable..."
        echo "=========================================================="
        ./build-linux/TestWindowPaint || status=$?
        echo "=========================================================="
        echo "Running Linux TestImage executable..."
        echo "=========================================================="
        ./build-linux/TestImage || status=$?
        echo "=========================================================="
        echo "Running Linux TestSvg executable..."
        echo "=========================================================="
        ./build-linux/TestSvg || status=$?
        echo "=========================================================="
        echo "Running Linux TestVideo executable..."
        echo "=========================================================="
        ./build-linux/TestVideo || status=$?
        echo "=========================================================="
        echo "Running Linux TestAudio executable..."
        echo "=========================================================="
        ./build-linux/TestAudio || status=$?
        echo "=========================================================="
        echo "Running Linux TestJs executable..."
        echo "=========================================================="
        ./build-linux/TestJs || status=$?
        echo "=========================================================="
        echo "Running Linux TestBase64 executable..."
        echo "=========================================================="
        ./build-linux/TestBase64 || status=$?
        return $status
      }
    '';
  }
)
