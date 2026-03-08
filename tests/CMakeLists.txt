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

# ── Phase 2: Execution tests ─────────────────────────────────────────────

# ProcessHandle + BoundedQueue tests
add_executable(test_process_handle
    unit/exec/process_handle_test.cpp
)
target_link_libraries(test_process_handle PRIVATE kairos_test_helpers)
gtest_discover_tests(test_process_handle)

# EnvBuilder tests
add_executable(test_env_builder
    unit/exec/env_builder_test.cpp
)
target_link_libraries(test_env_builder PRIVATE kairos_test_helpers)
gtest_discover_tests(test_env_builder)

# RunnerPool tests
add_executable(test_runner_pool
    unit/exec/runner_pool_test.cpp
)
target_link_libraries(test_runner_pool PRIVATE kairos_test_helpers)
gtest_discover_tests(test_runner_pool)

# ── Phase 2: Persistence tests ───────────────────────────────────────────

# DBWriter + QueryReader tests
add_executable(test_db_writer
    unit/persist/db_writer_test.cpp
)
target_link_libraries(test_db_writer PRIVATE kairos_test_helpers)
gtest_discover_tests(test_db_writer)

# ── Phase 2: Observability tests ─────────────────────────────────────────

# Metrics registry tests
add_executable(test_metrics
    unit/observability/metrics_test.cpp
)
target_link_libraries(test_metrics PRIVATE kairos_test_helpers)
gtest_discover_tests(test_metrics)

# ── Phase 3: Engine tests ────────────────────────────────────────────────

# FakeClock tests
add_executable(test_fake_clock
    unit/testing/fake_clock_test.cpp
)
target_link_libraries(test_fake_clock PRIVATE kairos_test_helpers)
gtest_discover_tests(test_fake_clock)

# TriggerEvent + ActiveRunTracker + Trigger types tests
add_executable(test_trigger_event
    unit/engine/trigger_event_test.cpp
)
target_link_libraries(test_trigger_event PRIVATE kairos_test_helpers)
gtest_discover_tests(test_trigger_event)

# DAG construction + topological sort tests
add_executable(test_dag
    unit/engine/dag_test.cpp
)
target_link_libraries(test_dag PRIVATE kairos_test_helpers)
gtest_discover_tests(test_dag)

# ExecutionPlan + WatchEventType + FakeFilesystem tests
add_executable(test_execution_plan
    unit/engine/execution_plan_test.cpp
)
target_link_libraries(test_execution_plan PRIVATE kairos_test_helpers)
gtest_discover_tests(test_execution_plan)

# Scheduler engine tests (component tests with FakeClock)
add_executable(test_scheduler
    unit/engine/scheduler_test.cpp
)
target_link_libraries(test_scheduler PRIVATE kairos_test_helpers)
gtest_discover_tests(test_scheduler)

# Pipeline engine tests (component tests with FakeProcess)
add_executable(test_pipeline
    unit/engine/pipeline_test.cpp
)
target_link_libraries(test_pipeline PRIVATE kairos_test_helpers)
gtest_discover_tests(test_pipeline)
