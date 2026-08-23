cmake_minimum_required(VERSION 3.20)
project(TextureToolkit CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

cmake_policy(SET CMP0091 NEW)

# Link the C runtime statically. This DLL is injected into arbitrary games on arbitrary machines;
# with the dynamic CRT a missing Visual C++ redistributable means the module simply fails to load,
# and since nothing of ours runs there is not even a log line to explain it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# Pinned so a build is reproducible: the same tag today and in a year. Overridable from the cache
# for anyone who needs to test against a newer one.
set(TT_IMGUI_TAG   "v1.92.5" CACHE STRING "Dear ImGui tag to build against")
set(TT_MINHOOK_TAG "v1.3.3"  CACHE STRING "MinHook tag to build against")

# Neither ships a usable CMake target for our purposes, so both are consumed as sources.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG ${TT_IMGUI_TAG}
    GIT_SHALLOW TRUE)

FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG ${TT_MINHOOK_TAG}
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(imgui minhook)

include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/reshade"
    "${minhook_SOURCE_DIR}/include"
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)

set(MINHOOK_SOURCES
    "${minhook_SOURCE_DIR}/src/buffer.c"
    "${minhook_SOURCE_DIR}/src/hook.c"
    "${minhook_SOURCE_DIR}/src/trampoline.c"
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND MINHOOK_SOURCES "${minhook_SOURCE_DIR}/src/hde/hde64.c")
else()
    list(APPEND MINHOOK_SOURCES "${minhook_SOURCE_DIR}/src/hde/hde32.c")
endif()

set(IMGUI_SOURCES
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
)

set(SOURCES
    src/Main.cpp
    src/Logger.cpp
    src/Config.cpp
    src/OSDBanner.cpp
    src/HookManager.cpp
    src/IATHook.cpp
    src/D3D9Hook.cpp
    src/D3D11Hook.cpp
    src/DInput8Hook.cpp
    src/TextureManager.cpp
    src/TextureToolkitUI.cpp
    src/TextureHash.cpp
    src/DDSLoader.cpp
    TextureToolkit.def
    ${MINHOOK_SOURCES}
    ${IMGUI_SOURCES}
)

add_library(TextureToolkit SHARED ${SOURCES})

# The architecture is in the file name: a 32-bit game needs the x86 build, and the two are
# indistinguishable once they are sitting in a plugins folder together.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(TT_ARCH "x64")
else()
    set(TT_ARCH "x86")
endif()

set_target_properties(TextureToolkit PROPERTIES
    PREFIX ""
    OUTPUT_NAME "TextureToolkit-${TT_ARCH}"
    SUFFIX ".asi"
)

target_compile_definitions(TextureToolkit PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    ImTextureID=ImU64
)

target_link_libraries(TextureToolkit PRIVATE Shlwapi d3d9 d3d11 dxgi)
