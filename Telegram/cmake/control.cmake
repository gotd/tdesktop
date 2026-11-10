# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

# gotd/tdesktop fork: local control server (docs/control.md).
# Compiled in by default, inert unless TDESKTOP_CONTROL=<socket> or
# "-control <socket>" is given at runtime.
option(TDESKTOP_CONTROL "Compile in the local control server." ON)
if (NOT TDESKTOP_CONTROL)
    return()
endif()

target_sources(Telegram
PRIVATE
    ${src_loc}/control/control.cpp
    ${src_loc}/control/control_app.cpp
    ${src_loc}/control/control_internal.h
    ${src_loc}/control/control_recorder.cpp
    ${src_loc}/control/control_widgets.cpp
)

# The static Qt of the Linux build environment ships the "offscreen"
# platform plugin but tdesktop imports only xcb and wayland. Link it in,
# so the client can run headless with QT_QPA_PLATFORM=offscreen. This is
# included before the Qt libraries are linked, so the plugin archive comes
# first on the link line.
option(TDESKTOP_CONTROL_OFFSCREEN "Link the Qt offscreen platform plugin." ON)
# Qt's imported plugin targets are local to the directory that found Qt,
# so locate the archive from the Qt6 package directory (<prefix>/lib/cmake/Qt6).
# The archive only exists for a static Qt (the docker build environment);
# a shared Qt loads its plugins at runtime anyway.
set(control_offscreen "")
if (TDESKTOP_CONTROL_OFFSCREEN
    AND LINUX
    AND Qt6_DIR)
    get_filename_component(control_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
    set(control_offscreen
        "${control_qt_prefix}/plugins/platforms/libqoffscreen.a")
    if (NOT EXISTS "${control_offscreen}")
        set(control_offscreen "")
    endif()
endif()
if (control_offscreen)
    target_link_libraries(Telegram PRIVATE "${control_offscreen}")
    # Only on the file that imports it: a target-wide definition would
    # rebuild every Telegram object when the option changes.
    set_source_files_properties(${src_loc}/control/control.cpp
    PROPERTIES
        COMPILE_DEFINITIONS TDESKTOP_CONTROL_OFFSCREEN
    )
    message(STATUS "Control: linking ${control_offscreen}")
endif()
