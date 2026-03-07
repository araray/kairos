# ╔════════════════════════════════════════════════════════════════════════════╗
# ║  Kairos Test Suite                                                        ║
# ╚════════════════════════════════════════════════════════════════════════════╝

include(GoogleTest)

# ── Shared test utilities ──────────────────────────────────────────────────
add_library(kairos_test_helpers INTERFACE)
target_include_directories(kairos_test_helpers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/helpers
)
target_link_libraries(kairos_test_helpers INTERFACE
    kairos_core
    kairos_platform
    GTest::gtest_main
)

# ── Unit tests ─────────────────────────────────────────────────────────────

# ID generation tests
add_executable(test_id_generator
    unit/core/id_generator_test.cpp
)
target_link_libraries(test_id_generator PRIVATE kairos_test_helpers)
gtest_discover_tests(test_id_generator)

# Config validation tests
add_executable(test_config
    unit/config/config_store_test.cpp
    unit/config/config_validation_test.cpp
)
target_link_libraries(test_config PRIVATE kairos_test_helpers)
gtest_discover_tests(test_config)

# Platform path tests
add_executable(test_platform
    unit/platform/path_normalize_test.cpp
)
target_link_libraries(test_platform PRIVATE kairos_test_helpers)
gtest_discover_tests(test_platform)

# SQLite persistence tests
add_executable(test_persist
    unit/persist/schema_migration_test.cpp
)
target_link_libraries(test_persist PRIVATE kairos_test_helpers)
gtest_discover_tests(test_persist)

# Exit codes test
add_executable(test_exit_codes
    unit/core/exit_codes_test.cpp
)
target_link_libraries(test_exit_codes PRIVATE kairos_test_helpers)
gtest_discover_tests(test_exit_codes)

# JSON formatter test
add_executable(test_json_formatter
    unit/observability/json_formatter_test.cpp
)
target_link_libraries(test_json_formatter PRIVATE kairos_test_helpers)
gtest_discover_tests(test_json_formatter)

# ── KEL (Kairos Expression Language) tests ────────────────────────────────

# KEL lexer tests
add_executable(test_kel_lexer
    unit/kel/lexer_test.cpp
)
target_link_libraries(test_kel_lexer PRIVATE kairos_test_helpers)
gtest_discover_tests(test_kel_lexer)

# KEL parser tests
add_executable(test_kel_parser
    unit/kel/parser_test.cpp
)
target_link_libraries(test_kel_parser PRIVATE kairos_test_helpers)
gtest_discover_tests(test_kel_parser)

# KEL evaluator tests
add_executable(test_kel_evaluator
    unit/kel/evaluator_test.cpp
)
target_link_libraries(test_kel_evaluator PRIVATE kairos_test_helpers)
gtest_discover_tests(test_kel_evaluator)
