# styled-qr-c as a single ESP-IDF component.
#
# Both translation units, one include directory. They are one library: qrstyle.h
# includes qrcodegen.h and the encode call takes a qrcodegen_Ecc, so splitting
# them into two components would only mean two ways to get the requirement
# wrong.
#
# Point IDF at esp-idf/ with EXTRA_COMPONENT_DIRS, or copy this directory into
# your project's components/.
#
# No definitions and no options: everything the library does is decided by the
# struct the caller fills in, at runtime, so a second style costs nothing at
# build time.

get_filename_component(STYLED_QR_SRC "${CMAKE_CURRENT_LIST_DIR}/../../src" ABSOLUTE)

if(NOT EXISTS "${STYLED_QR_SRC}/qrstyle.c")
    message(FATAL_ERROR "styled-qr-c sources not found at ${STYLED_QR_SRC}.")
endif()

idf_component_register(
    SRCS
        "${STYLED_QR_SRC}/qrcodegen.c"
        "${STYLED_QR_SRC}/qrstyle.c"
    INCLUDE_DIRS
        "${STYLED_QR_SRC}"
)
