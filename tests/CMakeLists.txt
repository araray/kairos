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

# ── Phase 3 Batch 13: CLI Table helper tests ───────────────────

# Table renderer, format_duration, truncate, status_icon, ANSI handling
add_executable(test_cli_table
    unit/cli/table_test.cpp
)
target_link_libraries(test_cli_table PRIVATE kairos_test_helpers)
gtest_discover_tests(test_cli_table)

# ── Phase 3 Batch 13: Metrics pruning + standalone_jobs tests ──

# PruneMetricsSnapshots handler, WorkflowRegistry::standalone_jobs()
add_executable(test_metrics_pruning
    unit/persist/metrics_pruning_test.cpp
)
target_link_libraries(test_metrics_pruning PRIVATE kairos_test_helpers)
gtest_discover_tests(test_metrics_pruning)

# ── Phase 3 Batch 14: Prune preview + events tail tests ─────

# PrunePreview, query_watch_events_since, query_max_event_id
add_executable(test_prune_preview
    unit/persist/prune_preview_test.cpp
)
target_link_libraries(test_prune_preview PRIVATE kairos_test_helpers)
gtest_discover_tests(test_prune_preview)

# ── Phase 3 Batch 14: Execution plan builder tests ──────────

# ExecutionPlan statistics, rendering, DAG-based plan construction
add_executable(test_explain_plan
    unit/engine/explain_plan_test.cpp
)
target_link_libraries(test_explain_plan PRIVATE kairos_test_helpers)
gtest_discover_tests(test_explain_plan)

# ── Phase 3 Batch 15: DB writer shutdown tests ──────────────

# DBWriter::stop() — graceful shutdown, statement release, idempotency
add_executable(test_db_writer_shutdown
    unit/persist/db_writer_shutdown_test.cpp
)
target_link_libraries(test_db_writer_shutdown PRIVATE kairos_test_helpers)
gtest_discover_tests(test_db_writer_shutdown)

# ── Phase 3 Batch 15: Runs cancel SQL tests ─────────────────

# Cancel SQL operations: RUNNING/PENDING → CANCELLED, no-op on completed
add_executable(test_runs_cancel
    unit/persist/runs_cancel_test.cpp
)
target_link_libraries(test_runs_cancel PRIVATE kairos_test_helpers)
gtest_discover_tests(test_runs_cancel)

# ── Phase 3 Batch 15: Shell completions structure tests ─────

# Validates completion script patterns and subcommand coverage
add_executable(test_completions
    unit/cli/completions_test.cpp
)
target_link_libraries(test_completions PRIVATE kairos_test_helpers)
gtest_discover_tests(test_completions)

# ══════════════════════════════════════════════════════════════════════════
# Phase 4 Batch 1: OutputMultiplexer, RunStream, CommandReader, HTTP server
# ══════════════════════════════════════════════════════════════════════════

# ── OutputMultiplexer tests ──────────────────────────────────

# Fan-out delivery, secret masking, remove sink, concurrency
add_executable(test_output_multiplexer
    unit/exec/output_sink_test.cpp
)
target_link_libraries(test_output_multiplexer PRIVATE kairos_test_helpers)
gtest_discover_tests(test_output_multiplexer)

# ── RunStream pub-sub tests ──────────────────────────────────

# Subscribe, publish, unsubscribe, close_run, concurrency
add_executable(test_run_stream
    unit/exec/run_stream_test.cpp
)
target_link_libraries(test_run_stream PRIVATE kairos_test_helpers)
gtest_discover_tests(test_run_stream)

# ── CommandReader tests ──────────────────────────────────────

# Command file parsing, polling, cleanup, response writing
add_executable(test_command_reader
    unit/daemon/command_reader_test.cpp
    ${CMAKE_SOURCE_DIR}/src/daemon/command_reader.cpp
)
target_link_libraries(test_command_reader PRIVATE kairos_test_helpers)
gtest_discover_tests(test_command_reader)

# ── HTTP server tests (conditional on KAIROS_HTTP) ───────────

add_executable(test_http_server
    unit/http/http_server_test.cpp
)
target_link_libraries(test_http_server PRIVATE kairos_test_helpers)
if(KAIROS_HTTP)
    target_compile_definitions(test_http_server PRIVATE KAIROS_HTTP_ENABLED)
    target_sources(test_http_server PRIVATE
        ${CMAKE_SOURCE_DIR}/src/http/http_server.cpp
    )
    target_link_libraries(test_http_server PRIVATE httplib::httplib)
    target_include_directories(test_http_server PRIVATE
        ${inja_SOURCE_DIR}/include
    )
endif()
gtest_discover_tests(test_http_server)

# ── Phase 4 Batch 2: CancelRegistry tests ──────────────────────

# Per-run cancellation, CombinedStopToken, concurrent cancel
add_executable(test_cancel_registry
    unit/engine/cancel_registry_test.cpp
)
target_link_libraries(test_cancel_registry PRIVATE kairos_test_helpers)
gtest_discover_tests(test_cancel_registry)

# ── Phase 4 Batch 2: MCP → RunStream wiring tests ──────────────

# start_log_follow subscription, base64 notifications, close_run
add_executable(test_mcp_log_follow_wiring
    unit/mcp/log_follow_wiring_test.cpp
)
target_link_libraries(test_mcp_log_follow_wiring PRIVATE kairos_test_helpers)
gtest_discover_tests(test_mcp_log_follow_wiring)

# ── Phase 4 Batch 2: RunStream on_close callback tests ─────────

# on_close fires, unsubscribe, multiple callbacks, exception safety
add_executable(test_run_stream_close
    unit/exec/run_stream_close_test.cpp
)
target_link_libraries(test_run_stream_close PRIVATE kairos_test_helpers)
gtest_discover_tests(test_run_stream_close)

# ── Phase 4 Batch 3: MCP tools wiring tests ────────────────────

# All 10 previously-stub MCP tools: listWorkflows, getWorkflow,
# runWorkflow, listJobs, runJob, queryRuns, getRunDetail,
# getRunLogs, getStepOutput, explainPlan + registry update + dispatch
add_executable(test_mcp_tools_wiring
    unit/mcp/mcp_tools_wiring_test.cpp
)
target_link_libraries(test_mcp_tools_wiring PRIVATE kairos_test_helpers)
gtest_discover_tests(test_mcp_tools_wiring)

# ── Phase 4 Batch 3: HTTP integration test (§30.6) ────────────

# E2E test: start HTTP server, test dashboard + API + auth + CORS
add_executable(test_http_integration
    integration/http_integration_test.cpp
)
target_link_libraries(test_http_integration PRIVATE kairos_test_helpers)
if(KAIROS_HTTP)
    target_compile_definitions(test_http_integration PRIVATE KAIROS_HTTP_ENABLED)
    target_sources(test_http_integration PRIVATE
        ${CMAKE_SOURCE_DIR}/src/http/http_server.cpp
    )
    target_link_libraries(test_http_integration PRIVATE httplib::httplib)
    # inja include path for template rendering.
    target_include_directories(test_http_integration PRIVATE
        ${inja_SOURCE_DIR}/include
    )
endif()
gtest_discover_tests(test_http_integration)

# ── Phase 4 Batch 3: OTelTracer tests (§21) ───────────────────

# OTelTracer / NullTracer — span creation, attributes, hierarchy
add_executable(test_otel_tracer
    unit/observability/otel_tracer_test.cpp
)
target_link_libraries(test_otel_tracer PRIVATE kairos_test_helpers)
if(KAIROS_OTEL)
    target_compile_definitions(test_otel_tracer PRIVATE KAIROS_OTEL)
    target_sources(test_otel_tracer PRIVATE
        ${CMAKE_SOURCE_DIR}/src/observability/otel_tracer.cpp
    )
    target_link_libraries(test_otel_tracer PRIVATE
        opentelemetry-cpp::trace
        opentelemetry-cpp::ostream_span_exporter
    )
    if(KAIROS_OTEL_OTLP)
        target_link_libraries(test_otel_tracer PRIVATE
            opentelemetry-cpp::otlp_http_exporter
        )
        target_compile_definitions(test_otel_tracer PRIVATE KAIROS_OTEL_OTLP)
    endif()
endif()
gtest_discover_tests(test_otel_tracer)

# ══════════════════════════════════════════════════════════════════════════
# Phase 5 Batch 1: SecretStore + Retention auto-prune
# ══════════════════════════════════════════════════════════════════════════

# ── SecretStore tests (SecureString, load_from_map, resolver, masking) ──

add_executable(test_secret_store
    unit/security/secret_store_test.cpp
)
target_link_libraries(test_secret_store PRIVATE kairos_test_helpers)
if(KAIROS_VAULT)
    target_compile_definitions(test_secret_store PRIVATE KAIROS_VAULT)
    target_link_libraries(test_secret_store PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
gtest_discover_tests(test_secret_store)

# ── Retention auto-prune tests (PruneOlderThan, PruneWatchSamples) ─────

add_executable(test_retention_auto_prune
    unit/persist/retention_auto_prune_test.cpp
)
target_link_libraries(test_retention_auto_prune PRIVATE kairos_test_helpers)
gtest_discover_tests(test_retention_auto_prune)

# ══════════════════════════════════════════════════════════════════════════
# Phase 5 Batch 2: Docker runner + E2E tests + Log formatter masking
# ══════════════════════════════════════════════════════════════════════════

# ── Docker runner tests ────────────────────────────────────────────────

add_executable(test_docker_runner
    unit/exec/docker_runner_test.cpp
)
target_link_libraries(test_docker_runner PRIVATE kairos_test_helpers)
if(KAIROS_DOCKER)
    target_compile_definitions(test_docker_runner PRIVATE KAIROS_DOCKER_ENABLED)
endif()
gtest_discover_tests(test_docker_runner)

# ── E2E daemon lifecycle tests (§30.6 Tier 4) ─────────────────────────

add_executable(test_e2e_daemon_lifecycle
    e2e/daemon_lifecycle_test.cpp
)
target_link_libraries(test_e2e_daemon_lifecycle PRIVATE kairos_test_helpers)
gtest_discover_tests(test_e2e_daemon_lifecycle
    PROPERTIES
        LABELS "e2e"
        TIMEOUT 120
)

# ── Migration tool tests (§31) ────────────────────────────────────────────

add_executable(test_migration
    unit/migration/migration_test.cpp
)
target_link_libraries(test_migration PRIVATE kairos_test_helpers)
gtest_discover_tests(test_migration
    PROPERTIES
        LABELS "migration"
        TIMEOUT 60
)
