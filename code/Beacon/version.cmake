# Generate version.h with the git description of the source tree.
# Runs on every build (see CMakeLists.txt); the file is rewritten only
# when the content changes, so it does not trigger needless rebuilds.
# Inputs: SRC (source dir), OUT (output header), VERSION (program version)

execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${SRC}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT GIT_VERSION)
    set(GIT_VERSION "unknown")
endif()

set(CONTENT "#define BEACON_VERSION \"${VERSION}\"\n#define BEACON_GIT_VERSION \"${GIT_VERSION}\"\n")
if(EXISTS ${OUT})
    file(READ ${OUT} OLD)
endif()
if(NOT "${OLD}" STREQUAL "${CONTENT}")
    file(WRITE ${OUT} "${CONTENT}")
endif()
