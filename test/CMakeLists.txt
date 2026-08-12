add_executable(platformchannel_test
    platformchannel_test.c
)

target_link_libraries(
    platformchannel_test
    flutterpi_module
    Unity
)

if (BUILD_WEBVIEW_CEF_PLUGIN)
    # flutterpi_module carries C++ objects (the CEF bridge) then, so the C++
    # runtime has to be linked in.
    set_target_properties(platformchannel_test PROPERTIES LINKER_LANGUAGE CXX)
endif()

add_test(platformchannel_test platformchannel_test)
