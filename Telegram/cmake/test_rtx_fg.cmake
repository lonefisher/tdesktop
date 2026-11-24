# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_executable(test_rtx_fg ${src_loc}/tests/test_rtx_fg.cpp)
target_compile_features(test_rtx_fg PRIVATE cxx_std_20)
target_include_directories(test_rtx_fg PRIVATE ${src_loc})
if (MSVC)
    target_compile_options(test_rtx_fg PRIVATE /W4 /EHsc /utf-8)
endif()

set_target_properties(test_rtx_fg PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/rtx_fg_test
)

add_dependencies(Telegram test_rtx_fg)

add_custom_command(TARGET test_rtx_fg POST_BUILD
    COMMAND $<TARGET_FILE:test_rtx_fg>
    VERBATIM
)
