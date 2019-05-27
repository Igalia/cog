# - Try to find wayland-scanner.
# Once done, this will define
#
#  WAYLAND_SCANNER_FOUND - system has Wayland.
#  WAYLAND_SCANNER - the path to the wayland-scanner binary
#  WAYLAND_SCANNER_CODE_ARG - the code argument to pass
#
# Copyright (C) 2019 Igalia S.L.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1.  Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
# 2.  Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND ITS CONTRIBUTORS ``AS
# IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR ITS
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
# OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

set(WAYLAND_SCANNER "" CACHE FILEPATH "Path to the wayland-scanner tool")

function(wayland_scanner_get_version _result _exepath)
    set(_version "")
    set(_status -1)
    if (EXISTS "${_exepath}")
        execute_process(
            COMMAND "${_exepath}" "--version"
            RESULT_VARIABLE _status
            ERROR_VARIABLE _version
            ERROR_STRIP_TRAILING_WHITESPACE
            OUTPUT_QUIET
        )
        if (_status EQUAL 0)
            string(REPLACE "wayland-scanner" "" _version "${_version}")
            string(STRIP "${_version}" _version)
        endif ()
    endif ()
    if (_version)
        set(${_result} "${_version}" PARENT_SCOPE)
    else ()
        unset(${_result} PARENT_SCOPE)
    endif ()
endfunction()

#
# Method 1: If -DWAYLAND_SCANNER=... was passed on the command line,
#           check whether we can extract the version number from it.
#           Otherwise: unset the variable, to try other methods below.
#
if (WAYLAND_SCANNER)
    wayland_scanner_get_version(WAYLAND_SCANNER_VERSION "${WAYLAND_SCANNER}")
    if (NOT WAYLAND_SCANNER_VERSION)
        set(WAYLAND_SCANNER "")
    endif ()
endif ()

#
# Method 2: Try to find an executable program in the current $PATH.
#
if (NOT WAYLAND_SCANNER)
    find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)
    if (WAYLAND_SCANNER_EXECUTABLE)
        wayland_scanner_get_version(WAYLAND_SCANNER_VERSION "${WAYLAND_SCANNER_EXECUTABLE}")
        if (WAYLAND_SCANNER_VERSION)
            set(WAYLAND_SCANNER "${WAYLAND_SCANNER_EXECUTABLE}")
        endif ()
    endif ()
    unset(WAYLAND_SCANNER_EXECUTABLE)
endif ()

#
# Method 3: Try to find the executable using pkg-config, in case the
#           tool was installed in a non-standard location.
#
if (NOT DEFINED WAYLAND_SCANNER OR NOT WAYLAND_SCANNER)
    find_package(PkgConfig)
    pkg_check_modules(WAYLAND_SCANNER_PC wayland-scanner)
    if (WAYLAND_SCANNER_PC_FOUND)
        pkg_get_variable(WAYLAND_SCANNER_PC_EXECUTABLE wayland-scanner wayland_scanner)
        if (WAYLAND_SCANNER_PC_EXECUTABLE)
            wayland_scanner_get_version(WAYLAND_SCANNER_VERSION "${WAYLAND_SCANNER_PC_EXECUTABLE}")
            if (WAYLAND_SCANNER_VERSION)
                set(WAYLAND_SCANNER "${WAYLAND_SCANNER_PC_EXECUTABLE}")
            endif ()
        endif ()
    endif ()
    unset(WAYLAND_SCANNER_PC)
    unset(WAYLAND_SCANNER_PC_EXECUTABLE)
endif ()

if (WAYLAND_SCANNER_VERSION)
    if (WAYLAND_SCANNER_VERSION VERSION_LESS "1.15")
        set(WAYLAND_SCANNER_CODE_ARG "code")
    else ()
        set(WAYLAND_SCANNER_CODE_ARG "private-code")
    endif ()
endif ()

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(
    WAYLAND_SCANNER
    DEFAULT_MSG
    WAYLAND_SCANNER
    WAYLAND_SCANNER_VERSION
    WAYLAND_SCANNER_CODE_ARG
)
