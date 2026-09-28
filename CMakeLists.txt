cmake_minimum_required(VERSION 3.20)
project(wt_rangefinder C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# --- SDL2: use the system package if present, otherwise build it from source ---
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
    message(STATUS "SDL2 not found on the system, fetching it")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-2.30.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL2)
endif()

if(TARGET SDL2::SDL2)
    set(SDL_LIB SDL2::SDL2)
else()
    set(SDL_LIB SDL2::SDL2-static)
endif()

# --- Dear ImGui (no CMake of its own, so we build the sources we need) ---
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC ${SDL_LIB})

# --- stb_image (header only) ---
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(stb)

# --- The app ---
add_executable(wt-rangefinder src/main.cpp)
target_include_directories(wt-rangefinder PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(wt-rangefinder PRIVATE imgui_lib ${SDL_LIB})

if(MSVC)
    # No console window on Windows, keep the standard main() entry point
    target_link_options(wt-rangefinder PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
elseif(MINGW)
    target_link_options(wt-rangefinder PRIVATE -mwindows)
endif()

# Copy maps.cfg and map images next to the binary
add_custom_command(TARGET wt-rangefinder POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:wt-rangefinder>)
