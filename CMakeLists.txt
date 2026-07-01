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

# Collect sources and headers
file(GLOB_RECURSE SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")

# Exclude main.cpp and BaseWindow.cpp from core library sources.
set(CORE_SRCS ${SRCS})
list(FILTER CORE_SRCS EXCLUDE REGEX "src/main.cpp$|src/core/BaseWindow.cpp$")

# Define Static Core Library
add_library(WebviewCore STATIC ${CORE_SRCS} ${HEADERS})
target_include_directories(WebviewCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/core
    ${CMAKE_CURRENT_SOURCE_DIR}/include/css
    ${CMAKE_CURRENT_SOURCE_DIR}/include/documents
    ${CMAKE_CURRENT_SOURCE_DIR}/include/elements
    ${CMAKE_CURRENT_SOURCE_DIR}/include/js
    ${CMAKE_CURRENT_SOURCE_DIR}/include/layout
    ${CMAKE_CURRENT_SOURCE_DIR}/include/multimedia
    ${CMAKE_CURRENT_SOURCE_DIR}/include/network
    ${CMAKE_CURRENT_SOURCE_DIR}/include/storage
)

# Propagate libxml2 dependencies
target_include_directories(WebviewCore PUBLIC ${LIBXML2_INCLUDE_DIRS})
target_link_libraries(WebviewCore PUBLIC ${LIBXML2_LIBRARIES})
target_compile_options(WebviewCore PUBLIC ${LIBXML2_CFLAGS_OTHER})

# Propagate OpenSSL dependencies
target_include_directories(WebviewCore PUBLIC ${OpenSSL_INCLUDE_DIRS})
target_link_libraries(WebviewCore PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# FFmpeg (libav*) optional video decoding
pkg_check_modules(LIBAV IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
if(LIBAV_FOUND)
    message(STATUS "FFmpeg found: enabling video decode (DWV_HAVE_FFMPEG)")
    target_include_directories(WebviewCore PUBLIC ${LIBAV_INCLUDE_DIRS})
    target_link_libraries(WebviewCore PUBLIC PkgConfig::LIBAV)
    target_compile_definitions(WebviewCore PUBLIC DWV_HAVE_FFMPEG)
else()
    message(STATUS "FFmpeg not found: video falls back to synthetic source")
endif()

# Find OpenCL (optional)
find_package(OpenCL QUIET)
if(OpenCL_FOUND)
    message(STATUS "OpenCL found: enabling GPU base64 (DWV_HAVE_OPENCL)")
    target_link_libraries(WebviewCore PUBLIC OpenCL::OpenCL)
    target_compile_definitions(WebviewCore PUBLIC DWV_HAVE_OPENCL)
else()
    message(STATUS "OpenCL not found: base64 uses CPU fallback")
endif()

if(WIN32)
    message(STATUS "Building for Windows Target...")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    set(WIN_LINK_OPTIONS -static-libgcc -static-libstdc++)
    target_link_libraries(WebviewCore PUBLIC user32 gdi32 ws2_32)
    target_link_options(WebviewCore PUBLIC ${WIN_LINK_OPTIONS})

    set(Vulkan_LIBRARIES vulkan-1)
    
    # Add executables
    add_executable(${PROJECT_NAME} src/main.cpp src/core/BaseWindow.cpp)
    target_link_libraries(${PROJECT_NAME} PRIVATE WebviewCore ${Vulkan_LIBRARIES} ole32)
    target_link_options(${PROJECT_NAME} PRIVATE ${WIN_LINK_OPTIONS})

    add_executable(TestWindowPaint test/test_windowpaint.cpp src/core/BaseWindow.cpp)
    target_link_libraries(TestWindowPaint PRIVATE WebviewCore ${Vulkan_LIBRARIES} ole32)
    target_link_options(TestWindowPaint PRIVATE ${WIN_LINK_OPTIONS})

    add_executable(TestAudio test/test_audio.cpp)
    target_link_libraries(TestAudio PRIVATE WebviewCore ole32)
    target_link_options(TestAudio PRIVATE ${WIN_LINK_OPTIONS})
else()
    message(STATUS "Building for Linux/X11 Target...")
    find_package(Vulkan REQUIRED)
    pkg_check_modules(X11 REQUIRED x11)
    pkg_check_modules(ALSA REQUIRED alsa)

    target_include_directories(WebviewCore PUBLIC ${X11_INCLUDE_DIRS} ${ALSA_INCLUDE_DIRS})
    target_link_libraries(WebviewCore PUBLIC ${X11_LIBRARIES} ${ALSA_LIBRARIES})

    # Main Application
    add_executable(${PROJECT_NAME} src/main.cpp src/core/BaseWindow.cpp)
    target_link_libraries(${PROJECT_NAME} PRIVATE WebviewCore Vulkan::Vulkan)

    # WindowPaint Test (uses BaseWindow.cpp)
    add_executable(TestWindowPaint test/test_windowpaint.cpp src/core/BaseWindow.cpp)
    target_link_libraries(TestWindowPaint PRIVATE WebviewCore Vulkan::Vulkan)

    # Audio Test
    add_executable(TestAudio test/test_audio.cpp)
    target_link_libraries(TestAudio PRIVATE WebviewCore)
endif()

# Plain Test Targets (macro to avoid redundancy)
macro(add_plain_test name file)
    add_executable(${name} ${file})
    target_link_libraries(${name} PRIVATE WebviewCore)
    if(WIN32)
        target_link_options(${name} PRIVATE ${WIN_LINK_OPTIONS})
    endif()
endmacro()

add_plain_test(TestNet test/test_net.cpp)
add_plain_test(TestWrapper test/test_wrapper.cpp)
add_plain_test(TestCss test/test_css.cpp)
add_plain_test(TestInput test/test_input.cpp)
add_plain_test(TestLayout test/test_layout.cpp)
add_plain_test(TestPaint test/test_paint.cpp)
add_plain_test(TestImage test/test_image.cpp)
add_plain_test(TestSvg test/test_svg.cpp)
add_plain_test(TestVideo test/test_video.cpp)
add_plain_test(TestFont test/test_font.cpp)
add_plain_test(TestBrowser test/test_browser.cpp)
add_plain_test(TestPdf test/test_pdf.cpp)
add_plain_test(TestJs test/test_js.cpp)
add_plain_test(TestBase64 test/test_base64.cpp)
add_plain_test(TestOptimizer test/test_optimizer.cpp)
