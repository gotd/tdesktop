# gotd/tdesktop fork: teled (https://github.com/gotd/teled) integration.
#
# Included from the end of td_mtproto.cmake, so all fork-specific sources and
# options live here instead of in upstream lists. See docs/teled.md.
#
# Options:
#   TDESKTOP_TELED_SERVER        default DC endpoints, "[dc@]host:port" list
#   TDESKTOP_TELED_RSA_KEY_FILE  PEM file with the server public key(s)
#                                embedded into the binary

set(TDESKTOP_TELED_SERVER "127.0.0.1:10443,[::1]:10443" CACHE STRING
    "Default teled endpoints, comma-separated '[dc@]host:port' or '[dc@][ipv6]:port'.")
set(TDESKTOP_TELED_RSA_KEY_FILE "" CACHE FILEPATH
    "PEM file with teled RSA public key(s) to embed. Empty: the teled testing key.")

set(teled_src_loc ${CMAKE_CURRENT_LIST_DIR}/../SourceFiles)
get_filename_component(teled_src_loc ${teled_src_loc} REALPATH)

if (TDESKTOP_TELED_RSA_KEY_FILE)
    get_filename_component(teled_key_file ${TDESKTOP_TELED_RSA_KEY_FILE} ABSOLUTE)
else()
    set(teled_key_file ${teled_src_loc}/mtproto/teled/teled_test_public_key.pem)
endif()

if (NOT EXISTS ${teled_key_file})
    message(FATAL_ERROR "TDESKTOP_TELED_RSA_KEY_FILE: '${teled_key_file}' not found.")
endif()
file(READ ${teled_key_file} TELED_BUILD_RSA_KEYS)
if (TELED_BUILD_RSA_KEYS MATCHES "PRIVATE KEY")
    message(FATAL_ERROR
        "TDESKTOP_TELED_RSA_KEY_FILE: '${teled_key_file}' contains a private key. "
        "Embed only the public key: openssl rsa -in key.pem -RSAPublicKey_out")
endif()
if (NOT TELED_BUILD_RSA_KEYS MATCHES "-----BEGIN (RSA )?PUBLIC KEY-----")
    message(FATAL_ERROR
        "TDESKTOP_TELED_RSA_KEY_FILE: '${teled_key_file}' has no "
        "'BEGIN RSA PUBLIC KEY' or 'BEGIN PUBLIC KEY' block.")
endif()
if (TELED_BUILD_RSA_KEYS MATCHES "\\)teled\"")
    message(FATAL_ERROR "TDESKTOP_TELED_RSA_KEY_FILE: unexpected ')teled\"' sequence.")
endif()
# Reconfigure when the key file changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${teled_key_file})

set(TELED_BUILD_SERVER "${TDESKTOP_TELED_SERVER}")
if (NOT TELED_BUILD_SERVER MATCHES "^[][0-9A-Za-z.:@,; _-]*$")
    message(FATAL_ERROR "TDESKTOP_TELED_SERVER: unexpected characters in '${TELED_BUILD_SERVER}'.")
endif()

set(teled_gen_loc ${CMAKE_CURRENT_BINARY_DIR}/gen/teled)
configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/teled_build_defaults.h.in
    ${teled_gen_loc}/teled_build_defaults.h
    @ONLY
)
message(STATUS "teled: servers '${TELED_BUILD_SERVER}', key file '${teled_key_file}'")

target_sources(td_mtproto
PRIVATE
    ${teled_src_loc}/mtproto/teled/teled_config.cpp
    ${teled_src_loc}/mtproto/teled/teled_config.h
    ${teled_gen_loc}/teled_build_defaults.h
)
target_include_directories(td_mtproto PRIVATE ${teled_gen_loc})
target_compile_definitions(td_mtproto PRIVATE TDESKTOP_TELED_BUILD_DEFAULTS)
