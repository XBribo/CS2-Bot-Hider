set(PLUGIN_GIT_SHA "Local")
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE git_sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE git_result
        ERROR_QUIET
    )
    if(git_result EQUAL 0)
        set(PLUGIN_GIT_SHA "${git_sha}")
    endif()
endif()

get_filename_component(output_dir "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(CONFIGURE OUTPUT "${OUTPUT_FILE}" CONTENT
    "#pragma once\n#define GITHUB_SHA \"@PLUGIN_GIT_SHA@\"\n"
    @ONLY
)
