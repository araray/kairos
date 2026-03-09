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

# ── Phase 3 Batch 3: Watch engine tests ──────────────────────────────────

# Sample+diff unit tests
add_executable(test_watch_sample
    unit/watch/sample_test.cpp
)
target_link_libraries(test_watch_sample PRIVATE kairos_test_helpers)
gtest_discover_tests(test_watch_sample)

# Watch engine component tests (FakeFilesystem + FakeClock)
add_executable(test_watch_engine
    unit/watch/watch_engine_test.cpp
)
target_link_libraries(test_watch_engine PRIVATE kairos_test_helpers)
gtest_discover_tests(test_watch_engine)

# ── Integration tests ────────────────────────────────────────────────────

# End-to-end pipeline tests (schedule→run, watch→trigger)
add_executable(test_integration
    integration/integration_test.cpp
)
target_link_libraries(test_integration PRIVATE kairos_test_helpers)
gtest_discover_tests(test_integration)

# ── Phase 3 Batch 4: YAML loader tests ──────────────────────────────────

# YAML workflow/watch-group loader tests
add_executable(test_yaml_loader
    unit/config/yaml_loader_test.cpp
)
target_link_libraries(test_yaml_loader PRIVATE kairos_test_helpers)
gtest_discover_tests(test_yaml_loader)

# ── Phase 3 Batch 4: Real filesystem scanner tests ──────────────────────

# Production filesystem scanner tests
add_executable(test_real_scanner
    unit/watch/real_scanner_test.cpp
)
target_link_libraries(test_real_scanner PRIVATE kairos_test_helpers)
gtest_discover_tests(test_real_scanner)

# ── Phase 3 Batch 4: Cron trigger tests ─────────────────────────────────

# CronTrigger::next_fire_after via croncpp
add_executable(test_cron_trigger
    unit/engine/cron_trigger_test.cpp
)
target_link_libraries(test_cron_trigger PRIVATE kairos_test_helpers)
gtest_discover_tests(test_cron_trigger)

# ── Phase 3 Batch 4: KEL watch rule evaluation tests ────────────────────

# KEL condition evaluation in watch engine
add_executable(test_kel_rules
    unit/watch/kel_rules_test.cpp
)
target_link_libraries(test_kel_rules PRIVATE kairos_test_helpers)
gtest_discover_tests(test_kel_rules)

# ── Phase 3 Batch 5: Native watch backend tests ────────────────────────

# InotifyWatcher component tests (Linux only)
add_executable(test_inotify_watcher
    unit/watch/inotify_watcher_test.cpp
)
target_link_libraries(test_inotify_watcher PRIVATE kairos_test_helpers)
gtest_discover_tests(test_inotify_watcher)

# ── Phase 3 Batch 5: Watch persistence tests ───────────────────────────

# InsertWatchSample + InsertWatchEvent via DBWriter
add_executable(test_watch_persistence
    unit/watch/watch_persistence_test.cpp
)
target_link_libraries(test_watch_persistence PRIVATE kairos_test_helpers)
gtest_discover_tests(test_watch_persistence)

# ── Phase 3 Batch 5: Parallel pipeline execution tests ─────────────────

# Parallel level-by-level execution via Pipeline
add_executable(test_parallel_pipeline
    unit/engine/parallel_pipeline_test.cpp
)
target_link_libraries(test_parallel_pipeline PRIVATE kairos_test_helpers)
gtest_discover_tests(test_parallel_pipeline)

# ── Phase 3 Batch 6: Debounce buffer tests ─────────────────────────────

# DebounceBuffer coalescing and timing
add_executable(test_debounce_buffer
    unit/watch/debounce_buffer_test.cpp
)
target_link_libraries(test_debounce_buffer PRIVATE kairos_test_helpers)
gtest_discover_tests(test_debounce_buffer)

# ── Phase 3 Batch 6: KEL aggregate/previous tests ─────────────────────

# KEL aggregate() and previous() functions
add_executable(test_kel_aggregate
    unit/watch/kel_aggregate_test.cpp
)
target_link_libraries(test_kel_aggregate PRIVATE kairos_test_helpers)
gtest_discover_tests(test_kel_aggregate)

# ── Phase 3 Batch 6: YAML→daemon wiring integration test ──────────────

# Workflow/watch-group loading and registry construction
add_executable(test_yaml_daemon_wiring
    integration/yaml_daemon_wiring_test.cpp
)
target_link_libraries(test_yaml_daemon_wiring PRIVATE kairos_test_helpers)
gtest_discover_tests(test_yaml_daemon_wiring)

# ── Phase 3 Batch 7: Hash utility tests ───────────────────────────────

# MD5 + SHA-256 correctness (RFC 1321, FIPS 180-4 vectors)
add_executable(test_hash_util
    unit/watch/hash_util_test.cpp
)
target_link_libraries(test_hash_util PRIVATE kairos_test_helpers)
gtest_discover_tests(test_hash_util)

# ── Phase 3 Batch 7: Symlink cycle detection tests ───────────────────

# RealScanner cycle detection with symlinks
add_executable(test_symlink_cycle
    unit/watch/symlink_cycle_test.cpp
)
target_link_libraries(test_symlink_cycle PRIVATE kairos_test_helpers)
gtest_discover_tests(test_symlink_cycle)

# ── Phase 3 Batch 7: Hybrid mode tests ───────────────────────────────

# Native→debounce→targeted rescan→dedup
add_executable(test_hybrid_mode
    unit/watch/hybrid_mode_test.cpp
)
target_link_libraries(test_hybrid_mode PRIVATE kairos_test_helpers)
gtest_discover_tests(test_hybrid_mode)

# ── Phase 3 Batch 7: Config reload round-trip test ───────────────────

# YAML modify → reload → verify
add_executable(test_config_reload
    integration/config_reload_test.cpp
)
target_link_libraries(test_config_reload PRIVATE kairos_test_helpers)
gtest_discover_tests(test_config_reload)

# ── Phase 3 Batch 8: Pattern matching tests ──────────────────────────

# Pattern regex matching in WatchEngine (§12.6.1)
add_executable(test_pattern_matching
    unit/watch/pattern_matching_test.cpp
)
target_link_libraries(test_pattern_matching PRIVATE kairos_test_helpers)
gtest_discover_tests(test_pattern_matching)

# ── Phase 3 Batch 8: Diagnostics endpoint tests ─────────────────────

# get_recent_events ring buffer + get_status (§12.14)
add_executable(test_diagnostics
    unit/watch/diagnostics_test.cpp
)
target_link_libraries(test_diagnostics PRIVATE kairos_test_helpers)
gtest_discover_tests(test_diagnostics)

# ── Phase 3 Batch 8: End-to-end watch engine test ───────────────────

# Real scanner + file mutations + pattern + hash + diagnostics
add_executable(test_e2e_watch
    integration/e2e_watch_test.cpp
)
target_link_libraries(test_e2e_watch PRIVATE kairos_test_helpers)
gtest_discover_tests(test_e2e_watch)

# ── Phase 3 Batch 9: MCP transport tests ────────────────────────────

# JSON-RPC 2.0 protocol compliance
add_executable(test_mcp_transport
    unit/mcp/transport_test.cpp
)
target_link_libraries(test_mcp_transport PRIVATE kairos_test_helpers)
gtest_discover_tests(test_mcp_transport)

# ── Phase 3 Batch 9: MCP handler tests ─────────────────────────────

# Tool dispatch, watch tools, protocol methods
add_executable(test_mcp_handler
    unit/mcp/handler_test.cpp
)
target_link_libraries(test_mcp_handler PRIVATE kairos_test_helpers)
gtest_discover_tests(test_mcp_handler)

# ── Phase 3 Batch 10: Tracer abstraction tests ────────────────────

# NullTracer, ScopedSpan, factory
add_executable(test_tracer
    unit/observability/tracer_test.cpp
)
target_link_libraries(test_tracer PRIVATE kairos_test_helpers)
gtest_discover_tests(test_tracer)

# ── Phase 3 Batch 10: Batch persistence + pruning tests ──────────

# BatchInsertWatchSamples, PruneWatchSamples
add_executable(test_batch_persistence
    unit/persist/batch_persistence_test.cpp
)
target_link_libraries(test_batch_persistence PRIVATE kairos_test_helpers)
gtest_discover_tests(test_batch_persistence)

# ── Phase 3 Batch 10: Watch events query test ─────────────────────

# QueryReader::query_watch_events
add_executable(test_watch_events_query
    unit/persist/watch_events_query_test.cpp
)
target_link_libraries(test_watch_events_query PRIVATE kairos_test_helpers)
gtest_discover_tests(test_watch_events_query)

# ── Phase 3 Batch 11: MCP log streaming tests ────────────────────

# Base64 encoding, log chunk notifications, run_complete, capabilities
add_executable(test_mcp_log_streaming
    unit/mcp/log_streaming_test.cpp
)
target_link_libraries(test_mcp_log_streaming PRIVATE kairos_test_helpers)
gtest_discover_tests(test_mcp_log_streaming)

# ── Phase 3 Batch 11: Run stats + ActiveRunTracker tests ─────────

# QueryReader::query_run_stats, query_db_size, total_active()
add_executable(test_run_stats
    unit/persist/run_stats_test.cpp
)
target_link_libraries(test_run_stats PRIVATE kairos_test_helpers)
gtest_discover_tests(test_run_stats)

# ── Phase 3 Batch 11: Watch scan span instrumentation tests ──────

# Recording tracer, span hierarchy, attributes
add_executable(test_watch_scan_spans
    unit/watch/watch_scan_spans_test.cpp
)
target_link_libraries(test_watch_scan_spans PRIVATE kairos_test_helpers)
gtest_discover_tests(test_watch_scan_spans)

# ── Phase 3 Batch 12: QueryReader CLI query tests ───────────────

# query_recent_runs, get_run_detail, get_log_chunks, get_run_summary,
# query_metrics_snapshots
add_executable(test_query_reader_cli
    unit/persist/query_reader_cli_test.cpp
)
target_link_libraries(test_query_reader_cli PRIVATE kairos_test_helpers)
gtest_discover_tests(test_query_reader_cli)

# ── Phase 3 Batch 12: Metrics snapshot persistence tests ────────

# snapshot_entries, InsertMetricsSnapshot, BatchInsertMetricsSnapshots
add_executable(test_metrics_snapshot
    unit/persist/metrics_snapshot_test.cpp
)
target_link_libraries(test_metrics_snapshot PRIVATE kairos_test_helpers)
gtest_discover_tests(test_metrics_snapshot)
