cmake_minimum_required(VERSION 3.16)

project(DesktopWebview CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json so editors/clangd resolve include paths.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find OpenSSL (required for both platforms)
find_package(OpenSSL REQUIRED)

# Find libxml2 (HTML parsing for the page wrapper)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBXML2 REQUIRED libxml-2.0)

# Collect sources and headers
file(GLOB_RECURSE SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")

# Main Application Target
add_executable(${PROJECT_NAME} ${SRCS} ${HEADERS})
target_include_directories(${PROJECT_NAME} PRIVATE include)

# Create a source list for tests (exclude main.cpp and BaseWindow.cpp)
set(TEST_SRCS ${SRCS})
list(FILTER TEST_SRCS EXCLUDE REGEX "src/main.cpp$|src/BaseWindow.cpp$")

# Net Test Target
add_executable(TestNet test/test_net.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestNet PRIVATE include)

# Wrapper Test Target (HTML parsing)
add_executable(TestWrapper test/test_wrapper.cpp ${TEST_SRCS} ${HEADERS})
target_include_directories(TestWrapper PRIVATE include)

# libxml2 is required by every target (the wrapper is part of the shared
# sources). Apply its include dirs and libraries uniformly.
foreach(_target ${PROJECT_NAME} TestNet TestWrapper)
    target_include_directories(${_target} PRIVATE ${LIBXML2_INCLUDE_DIRS})
    target_link_libraries(${_target} PRIVATE ${LIBXML2_LIBRARIES})
    target_compile_options(${_target} PRIVATE ${LIBXML2_CFLAGS_OTHER})
endforeach()

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

    # Links for main application
    target_link_libraries(${PROJECT_NAME} PRIVATE user32 gdi32 OpenSSL::SSL OpenSSL::Crypto ws2_32)
    target_link_options(${PROJECT_NAME} PRIVATE ${WIN_LINK_OPTIONS})

    # Links for the test targets
    foreach(_target TestNet TestWrapper)
        target_link_libraries(${_target} PRIVATE user32 gdi32 OpenSSL::SSL OpenSSL::Crypto ws2_32)
        target_link_options(${_target} PRIVATE ${WIN_LINK_OPTIONS})
    endforeach()
else()
    # Linux native build using X11
    message(STATUS "Building for Linux/X11 Target...")
    pkg_check_modules(X11 REQUIRED x11)

    # Links for main application
    target_include_directories(${PROJECT_NAME} PRIVATE ${OpenSSL_INCLUDE_DIRS} ${X11_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${X11_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto)

    # Links for the test targets
    foreach(_target TestNet TestWrapper)
        target_include_directories(${_target} PRIVATE ${OpenSSL_INCLUDE_DIRS})
        target_link_libraries(${_target} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    endforeach()
endif()