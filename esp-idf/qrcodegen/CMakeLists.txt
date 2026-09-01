# The vendored qrcodegen as an ESP-IDF component.
#
# Two files copied verbatim from upstream, so this is the whole of the build
# integration -- no configuration, no generated table, nothing to keep in step.
# See third_party/qrcodegen/README.md for the pin.
#
# Point IDF at esp-idf/ with EXTRA_COMPONENT_DIRS, or copy these two directories
# into your project's components/.

get_filename_component(QRCODEGEN_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/qrcodegen" ABSOLUTE)

if(NOT EXISTS "${QRCODEGEN_DIR}/qrcodegen.c")
    message(FATAL_ERROR "qrcodegen not found at ${QRCODEGEN_DIR}.")
endif()

idf_component_register(
    SRCS "${QRCODEGEN_DIR}/qrcodegen.c"
    INCLUDE_DIRS "${QRCODEGEN_DIR}"
)
