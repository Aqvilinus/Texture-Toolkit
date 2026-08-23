cmake_minimum_required(VERSION 3.20)
project(TextureToolkit CXX C)

# A bare clone builds with only CMake and a Visual Studio toolchain:
#   cmake -B build32 -A Win32 && cmake --build build32 --config Release

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT so the .asi needs no Visual C++ redistributable in the host process. Set before the
# dependencies are declared, or they link against the dynamic one and the build fails on __imp__.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

set(TT_IMGUI_TAG    "v1.92.5"  CACHE STRING "Dear ImGui tag to build against")
set(TT_MINHOOK_TAG  "v1.3.3"   CACHE STRING "MinHook tag to build against")
set(TT_DIRECTXTEX_TAG "may2026" CACHE STRING "DirectXTex tag to build against")

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG ${TT_IMGUI_TAG}
    GIT_SHALLOW TRUE)

FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG ${TT_MINHOOK_TAG}
    GIT_SHALLOW TRUE)

FetchContent_Declare(directxtex
    GIT_REPOSITORY https://github.com/microsoft/DirectXTex.git
    GIT_TAG ${TT_DIRECTXTEX_TAG}
    GIT_SHALLOW TRUE)

# imgui and minhook are consumed as sources -- neither ships a CMake target. DirectXTex does.
FetchContent_MakeAvailable(imgui minhook)

# DirectXTex cannot infer the architecture from a non-Visual-Studio generator.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(DIRECTX_ARCH x86 CACHE STRING "" FORCE)
else()
    set(DIRECTX_ARCH x64 CACHE STRING "" FORCE)
endif()

set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(BUILD_DX12 OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(directxtex)

set(IMGUI_SOURCES
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp")

set(MINHOOK_SOURCES
    "${minhook_SOURCE_DIR}/src/buffer.c"
    "${minhook_SOURCE_DIR}/src/hook.c"
    "${minhook_SOURCE_DIR}/src/trampoline.c")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND MINHOOK_SOURCES "${minhook_SOURCE_DIR}/src/hde/hde64.c")
else()
    list(APPEND MINHOOK_SOURCES "${minhook_SOURCE_DIR}/src/hde/hde32.c")
endif()

set(SOURCES
    src/core/config.cpp
    src/core/hook_manager.cpp
    src/core/iat_hook.cpp
    src/core/logger.cpp
    src/input/input_hook.cpp
    src/Main.cpp
    src/render/d3d11/d3d11_hook.cpp
    src/render/d3d11/d3d11_texture_manager.cpp
    src/render/d3d9/d3d9_format.cpp
    src/render/d3d9/d3d9_hook.cpp
    src/render/d3d9/d3d9_texture_manager.cpp
    src/render/dxgi/dxgi_format.cpp
    src/render/dxgi/dxgi_hook.cpp
    src/render/render_backend.cpp
    src/texture/texture_dump.cpp
    src/texture/texture_hash.cpp
    src/texture/texture_manager.cpp
    src/ui/osd_banner.cpp
    src/ui/overlay.cpp
    TextureToolkit.def
    ${MINHOOK_SOURCES}
    ${IMGUI_SOURCES})

add_library(TextureToolkit SHARED ${SOURCES})

set_target_properties(TextureToolkit PROPERTIES
    PREFIX ""
    OUTPUT_NAME "TextureToolkit"
    SUFFIX ".asi")

target_include_directories(TextureToolkit PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${minhook_SOURCE_DIR}/include"
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends")

target_compile_definitions(TextureToolkit PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    ImTextureID=ImU64)

# Namespaced from a package manager, plain as a subproject.
if(TARGET Microsoft::DirectXTex)
    set(TT_DIRECTXTEX_TARGET Microsoft::DirectXTex)
else()
    set(TT_DIRECTXTEX_TARGET DirectXTex)
endif()

target_link_libraries(TextureToolkit PRIVATE
    ${TT_DIRECTXTEX_TARGET}
    Shlwapi d3d9 d3d11 dxgi windowscodecs ole32 uuid)
