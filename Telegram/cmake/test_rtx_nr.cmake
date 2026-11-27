# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_executable(test_rtx_nr ${src_loc}/tests/test_rtx_nr.cpp)
target_compile_features(test_rtx_nr PRIVATE cxx_std_20)
target_include_directories(test_rtx_nr PRIVATE ${src_loc})
if (MSVC)
    target_compile_options(test_rtx_nr PRIVATE /W4 /EHsc /utf-8)
endif()

set_target_properties(test_rtx_nr PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/rtx_nr_test
)

add_dependencies(Telegram test_rtx_nr)

add_custom_command(TARGET test_rtx_nr POST_BUILD
    COMMAND $<TARGET_FILE:test_rtx_nr>
    VERBATIM
)
