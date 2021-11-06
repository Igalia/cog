find_package(GObjectIntrospection REQUIRED)
find_program(GI_DOCGEN
    NAMES gi-docgen gi-docgen.py
    HINTS "${CMAKE_SOURCE_DIR}/gi-docgen"
    DOC "Path to the gi-docgen program"
    REQUIRED
)

message(STATUS "Using g-ir-scanner: ${INTROSPECTION_SCANNER}")
message(STATUS "Using gi-docgen: ${GI_DOCGEN}")

configure_file(cog.toml.in cog.toml @ONLY)

# TODO: Probably should be moved somewhere else, and use the API
#       version string for other things, e.g. the library name.
set(COGCORE_API_VERSION "${PROJECT_VERSION_MAJOR}.0")
set(COGCORE_GIR_NAME "Cog-${COGCORE_API_VERSION}")
set(COGCORE_GIR "${CMAKE_CURRENT_BINARY_DIR}/${COGCORE_GIR_NAME}.gir")

function (get_target_doc_sources target varname)
    set(result "")
    get_property(sources TARGET ${target} PROPERTY SOURCES)
    get_property(srcdir TARGET ${target} PROPERTY SOURCE_DIR)
    foreach (src IN LISTS sources)
        get_filename_component(src "${src}" REALPATH BASE_DIR "${srcdir}")
        get_property(isobject SOURCE "${src}" PROPERTY EXTERNAL_OBJECT)
        get_property(location SOURCE "${src}" PROPERTY LOCATION)
        if (NOT isobject)
            list(APPEND result "${location}")
        endif ()
    endforeach ()
    set(${varname} ${result} PARENT_SCOPE)
endfunction ()

function (get_target_include_flags target varname)
    set(result "")
    get_property(dirs TARGET ${target} PROPERTY INCLUDE_DIRECTORIES)
    foreach (dir IN LISTS dirs)
        list(APPEND result "-I${dir}")
    endforeach ()
    set(${varname} ${result} PARENT_SCOPE)
endfunction ()

get_target_doc_sources(cogcore COGCORE_DOC_SOURCES)
get_target_include_flags(cogcore COGCORE_DOC_IFLAGS)

add_custom_command(
    OUTPUT "${COGCORE_GIR}"
    COMMENT "Generating ${COGCORE_GIR_NAME}.gir ..."
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    DEPENDS ${COGCORE_DOC_SOURCES}
    VERBATIM
    COMMAND CC="${CMAKE_C_COMPILER}"
        ${INTROSPECTION_SCANNER}
        --output=${COGCORE_GIR}
        --warn-all
        --no-libtool
        --library=$<TARGET_FILE_BASE_NAME:cogcore>
        --library-path=$<TARGET_FILE_DIR:cogcore>
        --nsversion=${COGCORE_API_VERSION}
        --c-include=cog/cog.h
        --namespace=Cog
        --identifier-prefix=Cog
        --symbol-prefix=cog
        --pkg-export=cogcore
        --pkg=${WPEWEBKIT_PC_NAME}
        --pkg=gobject-2.0
        --pkg=gio-2.0
        --pkg=glib-2.0
        ${COGCORE_DOC_IFLAGS}
        ${COGCORE_DOC_SOURCES}
)

file(GLOB COGCORE_DOC_MARKDOWN_SOURCES *.md)

add_custom_command(
    OUTPUT html/index.html
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Generating documentation for ${COGCORE_GIR_NAME} ..."
    DEPENDS cog.toml ${COGCORE_GIR} ${COGCORE_DOC_MARKDOWN_SOURCES}
    VERBATIM
    COMMAND "${GI_DOCGEN}" generate --quiet --no-namespace-dir
        --content-dir ${CMAKE_CURRENT_SOURCE_DIR}
        --output-dir ${CMAKE_CURRENT_BINARY_DIR}/html
        --config ${CMAKE_CURRENT_BINARY_DIR}/cog.toml
        ${COGCORE_GIR}
)

add_custom_target(doc ALL DEPENDS html/index.html)
