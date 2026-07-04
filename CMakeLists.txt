zephyr_include_directories(include)

if(CONFIG_PMW3610)
    target_sources(app PRIVATE src/pmw3610.c)
endif()

if(CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS)
    target_sources(app PRIVATE src/settings/pmw3610_settings.c)
endif()

if(CONFIG_ZMK_PMW3610_STUDIO_RPC)
    # When the driver core is not built (e.g. native_sim with no devicetree
    # node), still provide the public API as a stub returning zero devices so
    # the RPC handler links and works.
    if(NOT CONFIG_PMW3610)
        target_sources(app PRIVATE src/pmw3610_api_stub.c)
    endif()

    file(GLOB STUDIO_C_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/studio/*.c)
    target_sources(app PRIVATE ${STUDIO_C_FILES})

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
