cmake_minimum_required(VERSION 3.20)
project(BotHider VERSION 0.6.0 LANGUAGES CXX)

# Override the base version with -DPLUGIN_VERSION.
if(NOT DEFINED PLUGIN_VERSION)
    set(PLUGIN_VERSION "${PROJECT_VERSION}")
endif()
find_package(Git QUIET)
set(PLUGIN_BUILD_INFO "${CMAKE_BINARY_DIR}/generated/plugin_build_info.h")
add_custom_target(BotHider_build_info
    COMMAND "${CMAKE_COMMAND}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DOUTPUT_FILE=${PLUGIN_BUILD_INFO}"
        -P "${CMAKE_SOURCE_DIR}/cmake/UpdateBuildInfo.cmake"
    BYPRODUCTS "${PLUGIN_BUILD_INFO}"
    VERBATIM
)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format clang-format.exe)
set(BOT_HIDER_FORMAT_FILES "")
# Check formatting in project-owned sources.
foreach(extension c cc cpp cxx h hh hpp hxx inl)
    file(GLOB plugin_format_top CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.${extension}" "${CMAKE_SOURCE_DIR}/src/core/*.${extension}")
    file(GLOB_RECURSE plugin_format_nested CONFIGURE_DEPENDS LIST_DIRECTORIES false
        "${CMAKE_SOURCE_DIR}/src/features/*.${extension}" "${CMAKE_SOURCE_DIR}/src/bridge/*.${extension}"
        "${CMAKE_SOURCE_DIR}/src/utils/*.${extension}" "${CMAKE_SOURCE_DIR}/src/core/entity/*.${extension}")
    list(APPEND BOT_HIDER_FORMAT_FILES ${plugin_format_top} ${plugin_format_nested})
endforeach()

option(ENABLE_FORMAT_CHECK "Run clang-format check before building BotHider" OFF)

if(CLANG_FORMAT_EXECUTABLE)
    add_custom_target(format
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i --style=file ${BOT_HIDER_FORMAT_FILES}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Formatting BotHider sources"
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror --style=file ${BOT_HIDER_FORMAT_FILES}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Checking BotHider source formatting"
        VERBATIM
    )
else()
    add_custom_target(format
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format was not found. Set CLANG_FORMAT_EXECUTABLE."
        COMMAND ${CMAKE_COMMAND} -E false
    )
    add_custom_target(format-check
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format was not found. Set CLANG_FORMAT_EXECUTABLE."
        COMMAND ${CMAKE_COMMAND} -E false
    )
endif()

add_subdirectory(src/core/cs2_sdk)

add_library(
    BotHider SHARED
    src/plugin.cpp
    src/core/config.cpp
    src/core/interfaces.cpp
    src/core/gamedata.cpp
    src/features/avatar/avatar_override.cpp
    src/features/botinfo/bot_info.cpp
    src/features/clantag/clan_tag.cpp
    src/features/ping/ping_display.cpp
    src/features/identity/fake_client_manager.cpp
    src/features/identity/client_hooks.cpp
    src/features/identity/command_hooks.cpp
    src/features/identity/identity_hooks.cpp
    src/features/identity/identity_runtime.cpp
    src/features/identity/identity_state.cpp
    src/bridge/exports.cpp
    src/core/entity/entity_access.cpp
    src/core/offsets.cpp
)

target_link_libraries(BotHider PRIVATE cs2_sdk::sdk)
cs2_sdk_configure_plugin(BotHider)
add_dependencies(BotHider BotHider_build_info)

if(ENABLE_FORMAT_CHECK)
    add_dependencies(BotHider format-check)
endif()

target_include_directories(BotHider PRIVATE
    "${CMAKE_BINARY_DIR}/generated"
    src
    src/features/avatar
    src/features/botinfo
    src/features/clantag
    src/features/identity
    src/bridge
    src/core
    src/core/entity
    src/features/ping
    src/utils
)

target_compile_definitions(BotHider PRIVATE
    SEMVER="${PLUGIN_VERSION}"
)

if(WIN32)
    set(BOT_HIDER_PLATFORM_DIR win64)
    set(BOT_HIDER_SUFFIX ".dll")
else()
    set(BOT_HIDER_PLATFORM_DIR linuxsteamrt64)
    set(BOT_HIDER_SUFFIX ".so")
endif()

set(BOT_HIDER_PACKAGE_BIN_DIR "${CMAKE_BINARY_DIR}/package/addons/BotHider/bin/${BOT_HIDER_PLATFORM_DIR}")
set(BOT_HIDER_VDF_FILE "addons/BotHider/bin/${BOT_HIDER_PLATFORM_DIR}/BotHider")

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/BotHider.vdf.in"
    "${CMAKE_BINARY_DIR}/BotHider.vdf"
    @ONLY
)

set_target_properties(
    BotHider
    PROPERTIES
    OUTPUT_NAME "BotHider"
    PREFIX ""
    SUFFIX "${BOT_HIDER_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY         "${BOT_HIDER_PACKAGE_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${BOT_HIDER_PACKAGE_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${BOT_HIDER_PACKAGE_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY         "${BOT_HIDER_PACKAGE_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${BOT_HIDER_PACKAGE_BIN_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG   "${BOT_HIDER_PACKAGE_BIN_DIR}"
)

add_custom_target(BotHider_resources
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_BINARY_DIR}/package/addons/metamod"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_BINARY_DIR}/package/addons/BotHider"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/BotHider.vdf"
        "${CMAKE_BINARY_DIR}/package/addons/metamod/BotHider.vdf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/configs/addons/BotHider/bot_info.json"
        "${CMAKE_BINARY_DIR}/package/addons/BotHider/bot_info.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/configs/addons/BotHider/gamedata.json"
        "${CMAKE_BINARY_DIR}/package/addons/BotHider/gamedata.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/configs/addons/BotHider/config.json"
        "${CMAKE_BINARY_DIR}/package/addons/BotHider/config.json"
    VERBATIM
)
add_dependencies(BotHider BotHider_resources)
