# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_executable(test_rtx_vsr ${src_loc}/tests/test_rtx_vsr.cpp)
target_compile_features(test_rtx_vsr PRIVATE cxx_std_20)
target_include_directories(test_rtx_vsr PRIVATE ${src_loc})
if (MSVC)
    target_compile_options(test_rtx_vsr PRIVATE /W4 /EHsc /utf-8)
endif()

set_target_properties(test_rtx_vsr PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/rtx_vsr_test
)

add_dependencies(Telegram test_rtx_vsr)

add_custom_command(TARGET test_rtx_vsr POST_BUILD
    COMMAND $<TARGET_FILE:test_rtx_vsr>
    VERBATIM
)
