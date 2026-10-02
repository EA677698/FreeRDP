set(MODULE_NAME "TestSDL3")
set(MODULE_PREFIX "TEST_SDL3")

set(${MODULE_PREFIX}_DRIVER ${MODULE_NAME}.cpp)

set(${MODULE_PREFIX}_TESTS TestSDLVMonitor.cpp)

disable_warnings_for_directory(${CMAKE_CURRENT_BINARY_DIR})

create_test_sourcelist(${MODULE_PREFIX}_SRCS ${${MODULE_PREFIX}_DRIVER} ${${MODULE_PREFIX}_TESTS})

include_directories("${CMAKE_CURRENT_SOURCE_DIR}/..")
include_directories("${CMAKE_CURRENT_BINARY_DIR}/..")

add_executable(${MODULE_NAME} ${${MODULE_PREFIX}_SRCS} ../sdl_vmonitor.cpp)

target_link_libraries(${MODULE_NAME} PRIVATE freerdp)

set_target_properties(${MODULE_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${TESTING_OUTPUT_DIRECTORY}")

foreach(test ${${MODULE_PREFIX}_TESTS})
  get_filename_component(TestName ${test} NAME_WE)
  add_test(${TestName} ${TESTING_OUTPUT_DIRECTORY}/${MODULE_NAME} ${TestName})
endforeach()

set_property(TARGET ${MODULE_NAME} PROPERTY FOLDER "FreeRDP/Client/Test")
