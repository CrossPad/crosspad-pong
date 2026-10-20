# Pong -- ESP-IDF component.
#
# The one app of the pong firmware kit (crosspad-app.json "kits"): built into
# that image only. In any other kit it is a component with no sources, so the
# full firmware carries no Pong code -- every kB of an image is a kB of PSRAM
# (it is copied there at boot). The requirements stay the same either way:
# ESP-IDF reads them in an early pass that does not know the kit.
#
# The PC simulator does not read this file: it globs src/*.cpp (flat, as this
# file does).

set(_pong_srcs "")
set(_pong_kit "full")
if(DEFINED CROSSPAD_FW_KIT)
    set(_pong_kit "${CROSSPAD_FW_KIT}")
endif()
file(READ "${CMAKE_CURRENT_LIST_DIR}/crosspad-app.json" _pong_app_json)
string(JSON _pong_kit_count LENGTH "${_pong_app_json}" kits)
math(EXPR _pong_last "${_pong_kit_count} - 1")
foreach(_i RANGE ${_pong_last})
    string(JSON _k GET "${_pong_app_json}" kits ${_i})
    if(_k STREQUAL _pong_kit)
        file(GLOB _pong_srcs "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
    endif()
endforeach()

idf_component_register(
    SRCS ${_pong_srcs}
    REQUIRES
        crosspad-core
        crosspad-gui
        lvgl
)

if(_pong_srcs)
    # WHOLE_ARCHIVE keeps the REGISTER_APP-generated _register_Pong_app()
    # alive through linking -- main calls it via AppRegistry_InitAll().
    set_property(TARGET ${COMPONENT_LIB} PROPERTY WHOLE_ARCHIVE ON)
endif()
