cmake_minimum_required(VERSION 3.24)
# Windows: static MSVC runtime so SuperSaga.exe runs without installing the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(supersaga LANGUAGES C CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

option(SUPERSAGA_FETCH_SDL "Download and build SDL3 if it is not installed" ON)
option(SUPERSAGA_BUILD_TESTS "Build headless tests" ON)
option(SUPERSAGA_BUILD_TOOLS "Build analysis tools (supersaga_disasm, needs Capstone)" ON)
option(SUPERSAGA_MACOS_BUNDLE "Build SuperSaga.app (and the `dmg` target) on macOS" OFF)
option(SUPERSAGA_VENDOR_SDL "Always build SDL3 from source (static) instead of using a system install" OFF)
option(SUPERSAGA_PACKAGE "Add packaging targets (Windows zip, Linux AppDir install)" OFF)

# App icon renderer: the SuperSaga logo on a rounded tile; used for the macOS .icns, Windows .ico
# and the Linux icon. Reads the logo with stb_image (public domain, pinned).
include(FetchContent)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 4e9b368bfc071e97206ea5c6adba68dd0cbf8897)
FetchContent_MakeAvailable(stb)
add_executable(supersaga_icon tools/supersaga_icon/main.cpp)
target_include_directories(supersaga_icon SYSTEM PRIVATE ${stb_SOURCE_DIR})
target_compile_definitions(supersaga_icon PRIVATE SUPERSAGA_ICON_LOGO="${PROJECT_SOURCE_DIR}/assets/logo/supersaga-logo.png")
set_property(TARGET supersaga_icon APPEND PROPERTY OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/assets/logo/supersaga-logo.png")

# Core: ROM loading and verification. No SDL dependency, so it can be tested headless.
add_library(supersaga_core STATIC
    src/rom.cpp
    src/sha1.cpp
)
target_include_directories(supersaga_core PUBLIC src)
if(MSVC)
    target_compile_options(supersaga_core PRIVATE /W4)
else()
    target_compile_options(supersaga_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Translations of the launcher and menu text (src/app/i18n.h). Each assets/lang/<code>.po is
# compiled in; tools/i18n.py checks them against the code. No SDL.
set(supersaga_lang_cpp "")
foreach(pair "fr;fr" "de;de" "es;es" "it;it" "pt_br;pt-BR")
    list(GET pair 0 name)
    list(GET pair 1 file)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/lang_${name}.cpp")
    add_custom_command(OUTPUT "${out}"
        COMMAND "${CMAKE_COMMAND}" -DINPUT=${PROJECT_SOURCE_DIR}/assets/lang/${file}.po
                -DOUTPUT=${out} -DSYMBOL=supersaga_lang_${name} -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS assets/lang/${file}.po cmake/embed.cmake
        COMMENT "Embedding ${file}.po")
    list(APPEND supersaga_lang_cpp "${out}")
endforeach()
add_library(supersaga_i18n STATIC src/app/i18n.cpp ${supersaga_lang_cpp})
target_include_directories(supersaga_i18n PUBLIC src)
if(MSVC)
    target_compile_options(supersaga_i18n PRIVATE /W4)
else()
    target_compile_options(supersaga_i18n PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Save import/export and backups (no SDL).
add_library(supersaga_saves STATIC src/app/save_import.cpp src/app/fsutil.cpp)
target_include_directories(supersaga_saves PUBLIC src)
target_link_libraries(supersaga_saves PUBLIC supersaga_i18n)
if(WIN32)
    target_link_libraries(supersaga_saves PUBLIC shell32 ole32 uuid)
endif()
if(NOT MSVC)
    target_compile_options(supersaga_saves PRIVATE -Wall -Wextra)
endif()

# Save state slots (no SDL). Uses the system zlib when present (macOS/Linux); otherwise states
# are stored uncompressed.
add_library(supersaga_states STATIC src/app/states.cpp)
target_include_directories(supersaga_states PUBLIC src)
target_link_libraries(supersaga_states PUBLIC supersaga_gba supersaga_saves)
target_compile_definitions(supersaga_states PRIVATE SUPERSAGA_VERSION="${PROJECT_VERSION}")
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(supersaga_states PRIVATE ZLIB::ZLIB)
    target_compile_definitions(supersaga_states PRIVATE SUPERSAGA_HAVE_ZLIB=1)
endif()
if(NOT MSVC)
    target_compile_options(supersaga_states PRIVATE -Wall -Wextra)
endif()

# Updates from GitHub Releases: versions, release JSON, SHA-256, zip unpacking. No SDL.
add_library(supersaga_update_core STATIC src/update/sha256.cpp src/update/update_core.cpp)
target_include_directories(supersaga_update_core PUBLIC src)
if(ZLIB_FOUND)
    target_link_libraries(supersaga_update_core PRIVATE ZLIB::ZLIB)
    target_compile_definitions(supersaga_update_core PRIVATE SUPERSAGA_HAVE_ZLIB=1)
endif()
if(MSVC)
    target_compile_options(supersaga_update_core PRIVATE /W4)
else()
    target_compile_options(supersaga_update_core PRIVATE -Wall -Wextra)
endif()

# Display post-processing (scaling layout, GBA colors, frame blending, overlays). No SDL.
add_library(supersaga_display STATIC src/options/display.cpp)
target_include_directories(supersaga_display PUBLIC src)
if(MSVC)
    target_compile_options(supersaga_display PRIVATE /W4)
else()
    target_compile_options(supersaga_display PRIVATE -Wall -Wextra -Wpedantic)
endif()

# GBA runtime (interpreter, PPU, APU, BIOS HLE). No SDL dependency.
add_library(supersaga_gba STATIC
    src/gba/apu.cpp
    src/gba/bios.cpp
    src/gba/bus.cpp
    src/gba/cpu.cpp
    src/gba/ppu.cpp
    src/gba/state.cpp
    src/gba/system.cpp
    src/gba/wide_level.cpp
)
target_include_directories(supersaga_gba PUBLIC src)
if(MSVC)
    target_compile_options(supersaga_gba PRIVATE /W4)
else()
    target_compile_options(supersaga_gba PRIVATE -Wall -Wextra)
endif()

# SDL3: use a system install if present (brew install sdl3, pacman -S sdl3, vcpkg), else fetch it.
if(NOT SUPERSAGA_VENDOR_SDL)
    find_package(SDL3 CONFIG QUIET)
endif()
if(NOT SDL3_FOUND AND SUPERSAGA_FETCH_SDL)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.26
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
endif()

if(TARGET SDL3::SDL3)
    # Dear ImGui (core + SDL3 platform + SDL_Renderer3 backends), pinned.
    include(FetchContent)
    FetchContent_Declare(imgui_src
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.8
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(imgui_src)
    add_library(imgui STATIC
        ${imgui_src_SOURCE_DIR}/imgui.cpp
        ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_src_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_src_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_src_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
    )
    target_include_directories(imgui PUBLIC ${imgui_src_SOURCE_DIR} ${imgui_src_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC SDL3::SDL3)

    # RetroAchievements: rcheevos (MIT) for the rc_client runtime, and stb_image (public domain)
    # to decode PNG badges. Both pinned; built from source as plain C/C++ files.
    FetchContent_Declare(rcheevos
        GIT_REPOSITORY https://github.com/RetroAchievements/rcheevos.git
        GIT_TAG v12.5.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(rcheevos)
    set(rc ${rcheevos_SOURCE_DIR}/src)
    add_library(rcheevos STATIC
        ${rc}/rc_client.c ${rc}/rc_compat.c ${rc}/rc_util.c ${rc}/rc_version.c
        ${rc}/rapi/rc_api_common.c ${rc}/rapi/rc_api_info.c ${rc}/rapi/rc_api_runtime.c ${rc}/rapi/rc_api_user.c
        ${rc}/rcheevos/alloc.c ${rc}/rcheevos/condition.c ${rc}/rcheevos/condset.c ${rc}/rcheevos/consoleinfo.c
        ${rc}/rcheevos/format.c ${rc}/rcheevos/lboard.c ${rc}/rcheevos/memref.c ${rc}/rcheevos/operand.c
        ${rc}/rcheevos/rc_validate.c ${rc}/rcheevos/richpresence.c ${rc}/rcheevos/runtime.c
        ${rc}/rcheevos/runtime_progress.c ${rc}/rcheevos/trigger.c ${rc}/rcheevos/value.c
        ${rc}/rhash/md5.c)
    target_include_directories(rcheevos PUBLIC ${rcheevos_SOURCE_DIR}/include ${rc})
    target_include_directories(rcheevos SYSTEM PUBLIC ${stb_SOURCE_DIR})
    set_target_properties(rcheevos PROPERTIES C_STANDARD 99)

    # Options panel (owned by the options work; display.* / input.* settings).
    add_library(supersaga_options STATIC
        src/options/options_panel.cpp
        src/options/presenter.cpp
        src/options/input.cpp
    )
    target_include_directories(supersaga_options PUBLIC src)
    target_link_libraries(supersaga_options PUBLIC imgui supersaga_display supersaga_app supersaga_states SDL3::SDL3)

    # App layer: settings, launcher UI. Uses SDL (pref path, dialogs).
    add_library(supersaga_app STATIC
        src/app/config.cpp
    )
    target_include_directories(supersaga_app PUBLIC src)
    target_link_libraries(supersaga_app PUBLIC supersaga_i18n SDL3::SDL3)

    # Launcher: first-run ROM setup, themed menu, shortcuts, game session. The DM Sans font
    # (SIL OFL) and a few Lucide icons (ISC), both in assets/fonts, are compiled in.
    set(supersaga_font_cpp "")
    foreach(pair "dmsans_regular;DMSans-Regular.ttf" "dmsans_semibold;DMSans-SemiBold.ttf"
                 "dmsans_bold;DMSans-Bold.ttf" "lucide_icons;lucide-icons.ttf")
        list(GET pair 0 name)
        list(GET pair 1 file)
        set(out "${CMAKE_CURRENT_BINARY_DIR}/font_${name}.cpp")
        add_custom_command(OUTPUT "${out}"
            COMMAND "${CMAKE_COMMAND}" -DINPUT=${PROJECT_SOURCE_DIR}/assets/fonts/${file}
                    -DOUTPUT=${out} -DSYMBOL=supersaga_font_${name} -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
            DEPENDS assets/fonts/${file} cmake/embed.cmake
            COMMENT "Embedding ${file}")
        list(APPEND supersaga_font_cpp "${out}")
    endforeach()
    # The SuperSaga wordmark on the launcher card (assets/logo/supersaga-logo.png, original artwork).
    # Always embedded: an optional EXISTS check was only evaluated at configure time, so build
    # folders configured before the file was added never picked it up.
    set(out "${CMAKE_CURRENT_BINARY_DIR}/image_logo.cpp")
    add_custom_command(OUTPUT "${out}"
        COMMAND "${CMAKE_COMMAND}" -DINPUT=${PROJECT_SOURCE_DIR}/assets/logo/supersaga-logo.png -DOUTPUT=${out}
                -DSYMBOL=supersaga_image_logo -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS assets/logo/supersaga-logo.png cmake/embed.cmake
        COMMENT "Embedding the SuperSaga logo")
    list(APPEND supersaga_font_cpp "${out}")
    # The app icon (512x512, rendered by supersaga_icon), written into the shortcuts the launcher
    # creates so they carry the SuperSaga icon even from a plain build/supersaga.
    set(icon_png "${CMAKE_CURRENT_BINARY_DIR}/supersaga-icon-512.png")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/image_icon.cpp")
    add_custom_command(OUTPUT "${out}"
        COMMAND supersaga_icon --png 512 "${icon_png}"
        COMMAND "${CMAKE_COMMAND}" -DINPUT=${icon_png} -DOUTPUT=${out}
                -DSYMBOL=supersaga_image_icon -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS supersaga_icon cmake/embed.cmake
        COMMENT "Embedding the SuperSaga app icon")
    list(APPEND supersaga_font_cpp "${out}")
    # License texts compiled in (shown in F1 > About and by --licenses).
    foreach(pair "license;LICENSE" "notices;THIRD_PARTY_NOTICES.md" "dmsans_ofl;assets/fonts/DMSans-OFL.txt")
        list(GET pair 0 name)
        list(GET pair 1 file)
        set(out "${CMAKE_CURRENT_BINARY_DIR}/text_${name}.cpp")
        add_custom_command(OUTPUT "${out}"
            COMMAND "${CMAKE_COMMAND}" -DINPUT=${PROJECT_SOURCE_DIR}/${file} -DOUTPUT=${out}
                    -DSYMBOL=supersaga_text_${name} -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
            DEPENDS ${file} cmake/embed.cmake
            COMMENT "Embedding ${file}")
        list(APPEND supersaga_font_cpp "${out}")
    endforeach()
    add_library(supersaga_launcher STATIC
        src/app/achievements.cpp
        src/app/credentials.cpp
        src/app/http.cpp
        src/app/app.cpp
        src/app/platform.cpp
        src/app/theme.cpp
        src/app/theme_colors.cpp
        src/app/licenses.cpp
        src/app/toast.cpp
        ${supersaga_font_cpp}
    )
    target_include_directories(supersaga_launcher PUBLIC src)
    target_link_libraries(supersaga_launcher PUBLIC supersaga_options supersaga_gba supersaga_core supersaga_saves supersaga_states imgui SDL3::SDL3)
    target_link_libraries(supersaga_launcher PRIVATE rcheevos)
    target_compile_definitions(supersaga_launcher PRIVATE SUPERSAGA_VERSION="${PROJECT_VERSION}")
    if(APPLE)
        target_link_libraries(supersaga_launcher PRIVATE "-framework Security" "-framework CoreFoundation")
    elseif(WIN32)
        target_link_libraries(supersaga_launcher PRIVATE winhttp advapi32)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(supersaga_launcher PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
    endif()
    if(NOT MSVC)
        target_compile_options(supersaga_launcher PRIVATE -Wall -Wextra)
    endif()

    # In-app updater: downloads (WinHTTP / curl), install per OS, and its settings tab.
    add_library(supersaga_updater STATIC src/update/http.cpp src/update/updater.cpp src/update/update_ui.cpp)
    target_include_directories(supersaga_updater PUBLIC src)
    target_link_libraries(supersaga_updater PUBLIC supersaga_update_core supersaga_app imgui SDL3::SDL3)
    target_compile_definitions(supersaga_updater PUBLIC SUPERSAGA_VERSION="${PROJECT_VERSION}")
    if(WIN32)
        target_link_libraries(supersaga_updater PRIVATE winhttp)
    endif()
    if(NOT MSVC)
        target_compile_options(supersaga_updater PRIVATE -Wall -Wextra)
    endif()
    target_link_libraries(supersaga_launcher PUBLIC supersaga_updater)

    add_executable(supersaga src/main.cpp)
    if(WIN32)
        # GUI program (no console window), SuperSaga.exe with the app icon and version info.
        set(supersaga_ico "${CMAKE_CURRENT_BINARY_DIR}/supersaga.ico")
        add_custom_command(OUTPUT "${supersaga_ico}" COMMAND supersaga_icon --ico "${supersaga_ico}" DEPENDS supersaga_icon
                           COMMENT "Rendering supersaga.ico")
        configure_file(packaging/windows/supersaga.rc.in "${CMAKE_CURRENT_BINARY_DIR}/supersaga.rc" @ONLY)
        target_sources(supersaga PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/supersaga.rc" "${supersaga_ico}")
        set_target_properties(supersaga PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME SuperSaga)
        target_link_libraries(supersaga PRIVATE ole32 shell32 uuid)
    endif()
    target_link_libraries(supersaga PRIVATE supersaga_launcher supersaga_core supersaga_gba supersaga_options imgui SDL3::SDL3)
    if(APPLE AND SUPERSAGA_MACOS_BUNDLE)
        include(cmake/MacBundle.cmake)
        supersaga_configure_bundle(supersaga)
    elseif(APPLE)
        # Plain (unbundled) build/supersaga: embed an Info.plist so the menu bar says SuperSaga, not supersaga.
        set(supersaga_plist "${CMAKE_CURRENT_BINARY_DIR}/supersaga-embedded-Info.plist")
        file(WRITE "${supersaga_plist}" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\"><dict>\n"
            "<key>CFBundleName</key><string>SuperSaga</string>\n"
            "<key>CFBundleDisplayName</key><string>SuperSaga</string>\n"
            "<key>CFBundleShortVersionString</key><string>${PROJECT_VERSION}</string>\n"
            "</dict></plist>\n")
        target_link_options(supersaga PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${supersaga_plist}")
        set_property(TARGET supersaga APPEND PROPERTY LINK_DEPENDS "${supersaga_plist}")
    endif()
    if(SUPERSAGA_PACKAGE AND WIN32)
        # Release file: the single self-contained SuperSaga.exe (static runtime, SDL, zlib; licenses
        # embedded, see F1 > About and --licenses), copied under its release name.
        set(supersaga_release_exe "${CMAKE_CURRENT_BINARY_DIR}/SuperSaga-${PROJECT_VERSION}-Windows-x64.exe")
        add_custom_target(package_exe
            COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:supersaga>" "${supersaga_release_exe}"
            DEPENDS supersaga
            COMMENT "Building ${supersaga_release_exe}"
            VERBATIM)
    endif()
    if(SUPERSAGA_PACKAGE AND UNIX AND NOT APPLE)
        # Portable Linux binary: static libstdc++/libgcc; SDL is static and loads X11/Wayland/
        # ALSA/PulseAudio/PipeWire at runtime. Installed into an AppDir by the workflow.
        target_link_options(supersaga PRIVATE -static-libstdc++ -static-libgcc)
        set(supersaga_png "${CMAKE_CURRENT_BINARY_DIR}/supersaga.png")
        add_custom_command(OUTPUT "${supersaga_png}" COMMAND supersaga_icon --png 256 "${supersaga_png}" DEPENDS supersaga_icon
                           COMMENT "Rendering supersaga.png")
        add_custom_target(supersaga_linux_icon ALL DEPENDS "${supersaga_png}")
        install(TARGETS supersaga RUNTIME DESTINATION bin)
        install(FILES packaging/linux/supersaga.desktop DESTINATION share/applications)
        install(FILES "${supersaga_png}" DESTINATION share/icons/hicolor/256x256/apps)
        install(FILES LICENSE THIRD_PARTY_NOTICES.md packaging/README-Linux.txt DESTINATION share/doc/supersaga)
        install(FILES assets/fonts/DMSans-OFL.txt assets/fonts/Lucide-LICENSE.txt DESTINATION share/doc/supersaga)
    endif()
else()
    message(WARNING "SDL3 not found and SUPERSAGA_FETCH_SDL is OFF: only building supersaga_core")
endif()

if(SUPERSAGA_BUILD_TESTS)
    enable_testing()
    add_executable(supersaga_romcheck tests/romcheck.cpp)
    target_link_libraries(supersaga_romcheck PRIVATE supersaga_core)
    add_test(NAME sha1_selftest COMMAND supersaga_romcheck --selftest)
    add_executable(supersaga_display_test tests/display_test.cpp)
    target_link_libraries(supersaga_display_test PRIVATE supersaga_display)
    add_test(NAME display_options COMMAND supersaga_display_test)
    add_executable(supersaga_save_test tests/save_import_test.cpp)
    target_link_libraries(supersaga_save_test PRIVATE supersaga_saves)
    add_test(NAME save_import COMMAND supersaga_save_test)
    add_executable(supersaga_states_test tests/states_test.cpp)
    target_link_libraries(supersaga_states_test PRIVATE supersaga_states)
    add_test(NAME save_state_slots COMMAND supersaga_states_test)
    add_executable(supersaga_cheats_test tests/cheats_test.cpp)
    target_include_directories(supersaga_cheats_test PRIVATE src)
    add_test(NAME cheats COMMAND supersaga_cheats_test)
    add_executable(supersaga_i18n_test tests/i18n_test.cpp)
    target_link_libraries(supersaga_i18n_test PRIVATE supersaga_i18n)
    target_compile_definitions(supersaga_i18n_test PRIVATE SUPERSAGA_LANG_DIR="${PROJECT_SOURCE_DIR}/assets/lang")
    add_test(NAME translations COMMAND supersaga_i18n_test)
    add_executable(supersaga_update_test tests/update_test.cpp)
    target_link_libraries(supersaga_update_test PRIVATE supersaga_update_core)
    target_compile_definitions(supersaga_update_test PRIVATE SUPERSAGA_TEST_DATA="${PROJECT_SOURCE_DIR}/tests/data")
    add_test(NAME updates COMMAND supersaga_update_test)
    add_executable(supersaga_state_test tests/state_test.cpp)
    target_link_libraries(supersaga_state_test PRIVATE supersaga_gba supersaga_core)
    # Uses the user's own ROM when it sits in the source folder (it is never committed).
    target_compile_definitions(supersaga_state_test PRIVATE
        SUPERSAGA_TEST_ROM="${PROJECT_SOURCE_DIR}/Superstar Saga.gba"
        SUPERSAGA_TEST_BIOS="${PROJECT_SOURCE_DIR}/gba_bios.bin")
    add_test(NAME save_state_determinism COMMAND supersaga_state_test)
    set_tests_properties(save_state_determinism PROPERTIES SKIP_RETURN_CODE 77)
    if(TARGET supersaga_launcher)
        # RetroAchievements glue against canned server replies (offline).
        add_executable(supersaga_achievements_test tests/achievements_test.cpp)
        target_link_libraries(supersaga_achievements_test PRIVATE supersaga_launcher rcheevos)
        add_test(NAME achievements COMMAND supersaga_achievements_test)
        # Controller bindings driven through a virtual SDL gamepad.
        add_executable(supersaga_input_test tests/input_test.cpp)
        target_link_libraries(supersaga_input_test PRIVATE supersaga_launcher)
        add_test(NAME input COMMAND supersaga_input_test)
    endif()
endif()

# Analysis tools. Their output is derived from the ROM and goes to generated/ (gitignored).
if(SUPERSAGA_BUILD_TOOLS)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(CAPSTONE IMPORTED_TARGET capstone>=5)
    endif()
    if(TARGET PkgConfig::CAPSTONE)
        add_executable(supersaga_disasm tools/supersaga_disasm/main.cpp)
        target_link_libraries(supersaga_disasm PRIVATE supersaga_core PkgConfig::CAPSTONE)
        if(NOT MSVC)
            target_compile_options(supersaga_disasm PRIVATE -Wall -Wextra)
        endif()
    else()
        message(WARNING "Capstone not found via pkg-config: skipping supersaga_disasm (brew install capstone pkgconf)")
    endif()
endif()
