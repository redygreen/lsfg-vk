set(LAYER_SOURCES
    "src/entrypoint.cpp"
    "src/instance.cpp"
    "src/log.cpp"
    "src/swapchain.cpp")

find_package(Threads REQUIRED)

add_library(lsfg-vk-layer SHARED ${LAYER_SOURCES})

target_link_libraries(lsfg-vk-layer
    PUBLIC lsfg-vk-common
    PUBLIC lsfg-vk-backend
    PUBLIC Threads::Threads)

target_compile_options(lsfg-vk-layer PRIVATE
    -Wno-unknown-warning-option
    -Wno-unsafe-buffer-usage) # Array indexing

set_target_properties(lsfg-vk-layer PROPERTIES
    CXX_VISIBILITY_PRESET hidden)

configure_file(
    "VkLayer_LSFGVK_frame_generation.json.in"
    "VkLayer_LSFGVK_frame_generation.json"
    @ONLY)

install(TARGETS lsfg-vk-layer
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_LSFGVK_frame_generation.json"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d")

add_executable(lsfg-vk-adaptive-test src/adaptive_test.cpp)
target_include_directories(lsfg-vk-adaptive-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
