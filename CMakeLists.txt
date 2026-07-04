zephyr_include_directories(include)

if(CONFIG_PMW3610 OR CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS)
    # Per-device settings id resolution (src/pmw3610_settings_id.c) is used
    # by both the driver (GetInfo) and the settings module (per-device
    # setting keys); tiny and side-effect-free, so it is cheap to always
    # include whenever either consumer is built.
    target_sources(app PRIVATE src/pmw3610_settings_id.c)
endif()

if(CONFIG_PMW3610)
    target_sources(app PRIVATE src/pmw3610.c)
endif()

if(CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS)
    target_sources(app PRIVATE src/settings/pmw3610_settings.c)
endif()

if(CONFIG_ZMK_PMW3610_PROTOBUF)
    # When the driver core is not built (e.g. native_sim with no devicetree
    # node), still provide the public API as a stub returning zero devices so
    # consumers below link and work.
    if(NOT CONFIG_PMW3610)
        target_sources(app PRIVATE src/pmw3610_api_stub.c)
    endif()

    # Transport-independent request executor (GetInfo/ReadDiagnostics/
    # ReadRegister/WriteRegister against local devices): used by the Studio
    # RPC handler (src/studio/pmw3610_handler.c, CONFIG_ZMK_PMW3610_STUDIO_RPC,
    # central/non-split only) and by the split relay peripheral executor
    # (src/split/pmw3610_relay.c, CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY, which a
    # split peripheral -- with no Studio of its own -- also needs), so it does
    # not depend on either of those directly.
    target_sources(app PRIVATE src/studio/pmw3610_request_exec.c)

    list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
    include(nanopb)
    set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
    set(NANOPB_GENERATE_CPP_STANDALONE OFF)

    # NOTE: adding to app target directly causes build issues, so we create a library instead
    zephyr_library()
    file(GLOB_RECURSE PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/proto/*.proto)

    nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${CMAKE_CURRENT_SOURCE_DIR} ${PROTO_FILES})
    target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    target_sources(${ZEPHYR_CURRENT_LIBRARY} PRIVATE ${proto_srcs} ${proto_hdrs})

    # app should depend on the generated proto files
    target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/proto)
    add_dependencies(app ${ZEPHYR_CURRENT_LIBRARY})
endif()

if(CONFIG_ZMK_PMW3610_STUDIO_RPC)
    target_sources(app PRIVATE src/studio/pmw3610_handler.c)
endif()

if(CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY)
    target_sources(app PRIVATE src/split/pmw3610_relay.c)
endif()
