# qrstyle as an ESP-IDF component.
#
# PUBLIC on the requirement, because qrstyle.h includes qrcodegen.h -- a caller
# that includes one gets the other's types, and the encode call takes a
# qrcodegen_Ecc. Making it PRIVATE compiles this component and then fails in the
# caller, which is a worse place to find out.
#
# No definitions and no options: everything this library does is decided by the
# struct the caller fills in, at runtime, so a second style costs nothing at
# build time.

get_filename_component(QRSTYLE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src" ABSOLUTE)

idf_component_register(
    SRCS "${QRSTYLE_DIR}/qrstyle.c"
    INCLUDE_DIRS "${QRSTYLE_DIR}"
    REQUIRES qrcodegen
)
