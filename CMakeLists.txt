cmake_minimum_required(VERSION 3.16)

project(DesktopWebview CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json so editors/clangd resolve include paths.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Vendored single-header libraries (stb_image, stb_truetype) live in third_party/.
# src/stb_impl.cpp compiles their implementations once.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# Find OpenSSL (required for both platforms)
find_package(OpenSSL REQUIRED)

# Find libxml2 (HTML parsing for the page wrapper)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBXML2 REQUIRED libxml-2.0)

# Collect sources and headers (excluding any unused subdirectories if deleted)
file(GLOB_RECURSE SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")

# Main Application Target
add_executable(${PROJECT_NAME} ${SRCS} ${HEADERS})
target_include_directories(${PROJECT_NAME} PRIVATE include)

# Create a source list for tests. Exclude main.cpp and BaseWindow.cpp (window/
# Vulkan). Audio.cpp is included because Video.cpp (FfmpegVideoSource) now
# depends on Audio::AudioOutput for audio playback.
set(TEST_SRCS ${SRCS})
list(FILTER TEST_SRCS EXCLUDE REGEX "src/main.cpp$|src/BaseWindow.cpp$")

# Net Test Target
add_executable(TestNet test/test_net.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestNet PRIVATE include)

# Wrapper Test Target (HTML parsing)
add_executable(TestWrapper test/test_wrapper.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestWrapper PRIVATE include)

# CSS Test Target (parser, selectors, cascade)
add_executable(TestCss test/test_css.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestCss PRIVATE include)

# Layout Test Target (box model, block flow)
add_executable(TestLayout test/test_layout.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestLayout PRIVATE include)

# Paint Test Target (display list + software rasteriser)
add_executable(TestPaint test/test_paint.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestPaint PRIVATE include)

# Image Test Target (BMP/PPM decode + blit)
add_executable(TestImage test/test_image.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestImage PRIVATE include)

# SVG Test Target (libxml2 parse + Canvas rasteriser)
add_executable(TestSvg test/test_svg.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestSvg PRIVATE include)

# Video Test Target (frame source + timing)
add_executable(TestVideo test/test_video.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestVideo PRIVATE include)

# Font Test Target (bitmap font)
add_executable(TestFont test/test_font.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestFont PRIVATE include)

# Browser Test Target (orchestration: DOM->layout->paint->composite + chrome)
add_executable(TestBrowser test/test_browser.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestBrowser PRIVATE include)

# PDF Test Target (PDF parser + renderer)
add_executable(TestPdf test/test_pdf.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestPdf PRIVATE include)

# JS Engine Test Target
add_executable(TestJs test/test_js.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestJs PRIVATE include)

# Audio Test Target (ALSA/WASAPI PCM). Audio.cpp is filtered out of TEST_SRCS,
# so add it back here explicitly; only this target and the app need the backend.
add_executable(TestAudio test/test_audio.cpp ${TEST_SRCS} src/Audio.cpp ${HEADERS})
target_include_directories(TestAudio PRIVATE include)

# WindowPaint Test Target (Window + Paint + Layout + Css + Wrapper)
# Needs BaseWindow.cpp explicitly (it is filtered out of TEST_SRCS)
set(TestWindowPaint_SRCS ${TEST_SRCS} src/BaseWindow.cpp)
add_executable(TestWindowPaint test/test_windowpaint.cpp ${TestWindowPaint_SRCS} ${HEADERS})
target_include_directories(TestWindowPaint PRIVATE include)

# Base64 Test Target (encode/decode with OpenCL GPU path)
add_executable(TestBase64 test/test_base64.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestBase64 PRIVATE include)

# Optimizer Test Target (recallable OpenCL optimize passes + timing)
add_executable(TestOptimizer test/test_optimizer.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestOptimizer PRIVATE include)

set(ALL_TARGETS ${PROJECT_NAME} TestNet TestWrapper TestCss TestLayout TestPaint TestImage TestSvg TestVideo TestFont TestBrowser TestPdf TestAudio TestWindowPaint TestJs TestBase64 TestOptimizer)

# libxml2 is required by every target (the wrapper is part of the shared
# sources). Apply its include dirs and libraries uniformly.
foreach(_target ${ALL_TARGETS})
    target_include_directories(${_target} PRIVATE ${LIBXML2_INCLUDE_DIRS})
    target_link_libraries(${_target} PRIVATE ${LIBXML2_LIBRARIES})
    target_compile_options(${_target} PRIVATE ${LIBXML2_CFLAGS_OTHER})
endforeach()

# FFmpeg (libav*) for real video decoding. Optional: if not found (e.g. some
# cross toolchains), Video.cpp falls back to the synthetic source. The video
# decode path lives in src/Video.cpp behind DWV_HAVE_FFMPEG, which is part of
# the shared sources, so apply to every target when available.
pkg_check_modules(LIBAV IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
if(LIBAV_FOUND)
    message(STATUS "FFmpeg found: enabling video decode (DWV_HAVE_FFMPEG)")
    foreach(_target ${ALL_TARGETS})
        target_include_directories(${_target} PRIVATE ${LIBAV_INCLUDE_DIRS})
        target_link_libraries(${_target} PRIVATE PkgConfig::LIBAV)
        target_compile_definitions(${_target} PRIVATE DWV_HAVE_FFMPEG)
    endforeach()
else()
    message(STATUS "FFmpeg not found: video falls back to synthetic source")
endif()

# Find OpenCL (optional): GPU-accelerated base64 encode/decode.
find_package(OpenCL QUIET)
if(OpenCL_FOUND)
    message(STATUS "OpenCL found: enabling GPU base64 (DWV_HAVE_OPENCL)")
    foreach(_target ${ALL_TARGETS})
        target_link_libraries(${_target} PRIVATE OpenCL::OpenCL)
        target_compile_definitions(${_target} PRIVATE DWV_HAVE_OPENCL)
    endforeach()
else()
    message(STATUS "OpenCL not found: base64 uses CPU fallback")
endif()

if(WIN32)
    # Windows native or cross-compilation using MinGW
    message(STATUS "Building for Windows Target...")

    # Binaries (.exe) in bin/, DLLs in lib/, import libs in lib/
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    # Statically embed the GCC/C++ runtimes so the .exe does not need the
    # MinGW runtime DLLs. We intentionally do NOT use a full -static link:
    # libxml2 pulls in iconv, which the MinGW toolchain only provides as a DLL
    # (libiconv-2.dll). OpenSSL and iconv are therefore linked dynamically and
    # their DLLs are copied next to the executable at run time (see shell.nix).
    set(WIN_LINK_OPTIONS -static-libgcc -static-libstdc++)

    set(Vulkan_LIBRARIES vulkan-1)

    # Links for main application and TestWindowPaint (Vulkan; the app also pulls
    # in Audio.cpp, so it needs ole32 for WASAPI).
    foreach(_targetDesktop ${PROJECT_NAME} TestWindowPaint)
        target_link_libraries(${_targetDesktop} PRIVATE user32 gdi32 OpenSSL::SSL OpenSSL::Crypto ws2_32 ${Vulkan_LIBRARIES} ole32)
        target_link_options(${_targetDesktop} PRIVATE ${WIN_LINK_OPTIONS})
    endforeach()

    # Links for the plain test targets
    foreach(_target TestNet TestWrapper TestCss TestLayout TestPaint TestImage TestSvg TestVideo TestFont TestBrowser TestPdf TestJs TestBase64 TestOptimizer)
        target_link_libraries(${_target} PRIVATE user32 gdi32 OpenSSL::SSL OpenSSL::Crypto ws2_32)
        target_link_options(${_target} PRIVATE ${WIN_LINK_OPTIONS})
    endforeach()

    # Audio test: WASAPI needs ole32.
    target_link_libraries(TestAudio PRIVATE user32 gdi32 OpenSSL::SSL OpenSSL::Crypto ws2_32 ole32)
    target_link_options(TestAudio PRIVATE ${WIN_LINK_OPTIONS})
else()
    # Linux native build using X11
    message(STATUS "Building for Linux/X11 Target...")
    find_package(Vulkan REQUIRED)
    pkg_check_modules(X11 REQUIRED x11)
    pkg_check_modules(ALSA REQUIRED alsa)

    # Debugger.hpp (pulled in via Net.cpp, which is part of every target's
    # sources) provides a native X11-drawn alert dialog, so every Linux target
    # needs the X11 headers to compile and libX11 to link.
    foreach(_target ${ALL_TARGETS})
        target_include_directories(${_target} PRIVATE ${X11_INCLUDE_DIRS})
        target_link_libraries(${_target} PRIVATE ${X11_LIBRARIES})
    endforeach()

    # Links for main application and TestWindowPaint. The app includes Audio.cpp,
    # so it links the ALSA backend.
    foreach(_targetDesktop ${PROJECT_NAME} TestWindowPaint)
        target_include_directories(${_targetDesktop} PRIVATE ${OpenSSL_INCLUDE_DIRS} ${X11_INCLUDE_DIRS} ${ALSA_INCLUDE_DIRS})
        target_link_libraries(${_targetDesktop} PRIVATE ${X11_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto Vulkan::Vulkan ${ALSA_LIBRARIES})
    endforeach()

    # Links for the plain test targets (Audio.cpp is now part of TEST_SRCS,
    # so every target needs ALSA for audio playback support).
    foreach(_target TestNet TestWrapper TestCss TestLayout TestPaint TestImage TestSvg TestVideo TestFont TestBrowser TestPdf TestJs TestBase64 TestOptimizer TestAudio)
        target_include_directories(${_target} PRIVATE ${OpenSSL_INCLUDE_DIRS} ${ALSA_INCLUDE_DIRS})
        target_link_libraries(${_target} PRIVATE OpenSSL::SSL OpenSSL::Crypto ${ALSA_LIBRARIES})
    endforeach()
endif()