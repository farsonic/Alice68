# Build version string in Amiga-like manner: $VER: project_name version (build_date) git:git_hash"
set(ALICE68_VERSTRING_DIR ${CMAKE_CURRENT_LIST_DIR})   # Alice68: where alice_splash.h is
function(get_verstring OUTPUT)
    find_package(Git)
    string(TIMESTAMP PROJECT_DATE "(%d.%m.%Y)")
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} OUTPUT_VARIABLE GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} OUTPUT_VARIABLE GIT_PORCELAIN OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (NOT GIT_PORCELAIN STREQUAL "")
        set(GIT_PORCELAIN ",dirty")
    endif()
    # Alice68: the kernel names itself Alice68 (ALICE_VERSION in alice_splash.h), based on
    # Emu68 ${PROJECT_VERSION}. The /emu68 device-tree node and its numeric "version" stay
    # Emu68's: the 68k drivers written for Emu68 look for them.
    # (the PowerPC ROM's own project includes this file too: found from where this file is)
    set(ALICE_V "")
    set(ALICE_H ${ALICE68_VERSTRING_DIR}/../src/raspi/alice_splash.h)
    if (EXISTS ${ALICE_H})
        file(STRINGS ${ALICE_H} ALICE_V REGEX "^#define ALICE_VERSION ")
    endif()
    if (ALICE_V)
        string(REGEX REPLACE "^[^\"]*\"([^\"]*)\".*$" "\\1" ALICE_V "${ALICE_V}")
        set(${OUTPUT} "$VER: Alice68 ${ALICE_V} ${PROJECT_DATE} git:${GIT_HASH}${GIT_PORCELAIN}, based on Emu68 ${PROJECT_VERSION}" PARENT_SCOPE)
    else()
        set(${OUTPUT} "$VER: ${PROJECT_NAME} ${PROJECT_VERSION} ${PROJECT_DATE} git:${GIT_HASH}${GIT_PORCELAIN}" PARENT_SCOPE)
    endif()
endfunction(get_verstring)

function(get_git_sha OUTPUT)
    find_package(Git)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} OUTPUT_VARIABLE GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${OUTPUT} "${GIT_HASH}" PARENT_SCOPE)
endfunction(get_git_sha)
