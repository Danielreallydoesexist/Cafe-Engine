cmake_minimum_required(VERSION 3.20)
project(CafeEngine VERSION 0.2.0 LANGUAGES CXX)

# The devkitPro Wii U wrapper loads WUT's CMake helpers before this project is
# configured. Keep platform selection automatic so the desktop command remains
# unchanged and powerpc-eabi-cmake creates the native target.
if(COMMAND wut_create_rpx)
    set(CAFE_WIIU ON)
else()
    set(CAFE_WIIU OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PNG REQUIRED)
find_package(SDL2 CONFIG REQUIRED)

add_library(cafe_engine
    src/core/Engine.cpp
    src/core/File.cpp
    src/uml/UmlParser.cpp
    src/cstyle/Style.cpp
    src/cafescript/Interpreter.cpp
    src/renderer/SoftwareRenderer.cpp
    src/renderer/Font.cpp
    src/renderer/EmbeddedFontData.cpp
    src/assets/AssetManager.cpp
    src/assets/CaveGenerator.cpp
    src/assets/ProceduralGenerator.cpp
    src/assets/Vorbis.cpp
    src/audio/AudioSystem.cpp
)
target_include_directories(cafe_engine PUBLIC include)
target_include_directories(cafe_engine PRIVATE third_party/stb)
target_link_libraries(cafe_engine PUBLIC PNG::PNG SDL2::SDL2)
set_source_files_properties(src/assets/Vorbis.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>")
target_compile_options(cafe_engine PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

if(CAFE_WIIU)
    option(CAFE_RENDER_PROFILE "Log Wii U renderer counters every 120 frames" OFF)
    add_executable(cafe
        src/platform/wiiu/main.cpp
        src/renderer/WiiURenderer.cpp
    )
    target_compile_definitions(cafe PRIVATE CAFE_PLATFORM_WIIU=1)
    if(CAFE_RENDER_PROFILE)
        target_compile_definitions(cafe PRIVATE CAFE_RENDER_PROFILE=1)
    endif()
    # WUT links Cafe OS imports through its RPX specs; these are not normal
    # static libraries and must not be added as -lwhb/-lcoreinit/-lvpad.
    target_link_libraries(cafe PRIVATE cafe_engine)
    wut_create_rpx(cafe)

    include(cmake/CafeWiiUGames.cmake)
    cafe_add_wiiu_games("${CMAKE_CURRENT_SOURCE_DIR}/examples")

    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cafe.rpx"
            DESTINATION cafe_engine)
    install(FILES platform/wiiu/meta.xml
            DESTINATION cafe_engine)
else()
    if(APPLE)
        enable_language(OBJCXX)
    endif()
    add_executable(cafe src/main.cpp)
    target_link_libraries(cafe PRIVATE cafe_engine)
    if(APPLE)
        target_sources(cafe PRIVATE src/platform/desktop/MacWindow.mm)
        target_link_libraries(cafe PRIVATE "-framework Cocoa" "-framework GameController")
        set_source_files_properties(src/platform/desktop/MacWindow.mm PROPERTIES
            COMPILE_FLAGS "-fobjc-arc")
        target_compile_definitions(cafe PRIVATE CAFE_DESKTOP_WINDOW=1)
    endif()
endif()

include(CTest)
if(BUILD_TESTING AND NOT CAFE_WIIU)
    add_executable(cafe_tests tests/tests.cpp)
    target_link_libraries(cafe_tests PRIVATE cafe_engine)
    target_compile_definitions(cafe_tests PRIVATE CAFE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME cafe_tests COMMAND cafe_tests)
endif()
