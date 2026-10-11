# ╔════════════════════════════════════════════════════════════════════════════╗
# ║  CheckOtelSystemDeps.cmake — Validate system OTel SDK runtime deps       ║
# ║                                                                           ║
# ║  After find_package(opentelemetry-cpp) succeeds, the system SDK's        ║
# ║  shared libraries may have dangling transitive dependencies              ║
# ║  (e.g. libprotobuf.so.NN after an OS upgrade).  This module runs a      ║
# ║  quick platform-native check to catch broken deps *at configure time*    ║
# ║  instead of at runtime.                                                   ║
# ║                                                                           ║
# ║  Sets KAIROS_OTEL_SYSTEM_DEPS_OK to TRUE / FALSE.                       ║
# ╚════════════════════════════════════════════════════════════════════════════╝

function(kairos_check_otel_system_deps)
    set(KAIROS_OTEL_SYSTEM_DEPS_OK TRUE PARENT_SCOPE)

    # Only meaningful for shared library installations.
    # Try to locate the OTel trace library to inspect.
    get_target_property(_otel_lib opentelemetry-cpp::trace
                        IMPORTED_LOCATION)
    if(NOT _otel_lib)
        get_target_property(_otel_lib opentelemetry-cpp::trace
                            IMPORTED_LOCATION_RELEASE)
    endif()
    if(NOT _otel_lib)
        get_target_property(_otel_lib opentelemetry-cpp::trace
                            IMPORTED_LOCATION_NOCONFIG)
    endif()

    if(NOT _otel_lib OR NOT EXISTS "${_otel_lib}")
        # Can't inspect — assume OK (might be static or interface target).
        return()
    endif()

    # --- Linux: use ldd -------------------------------------------------------
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_program(_ldd ldd)
        if(_ldd)
            execute_process(
                COMMAND ${_ldd} "${_otel_lib}"
                OUTPUT_VARIABLE _ldd_out
                ERROR_VARIABLE  _ldd_err
                RESULT_VARIABLE _ldd_rc
                TIMEOUT 5
            )
            if(_ldd_out MATCHES "not found")
                message(WARNING
                    "System OTel SDK has broken transitive dependencies:\n"
                    "${_ldd_out}\n"
                    "Falling back to FetchContent build (ostream exporter, no OTLP)."
                )
                set(KAIROS_OTEL_SYSTEM_DEPS_OK FALSE PARENT_SCOPE)
                return()
            endif()
        endif()
    endif()

    # --- macOS: use otool -----------------------------------------------------
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        find_program(_otool otool)
        if(_otool)
            execute_process(
                COMMAND ${_otool} -L "${_otel_lib}"
                OUTPUT_VARIABLE _otool_out
                ERROR_VARIABLE  _otool_err
                RESULT_VARIABLE _otool_rc
                TIMEOUT 5
            )
            # otool doesn't have "not found" the same way; check rc.
            if(NOT _otool_rc EQUAL 0)
                message(WARNING
                    "System OTel SDK library inspection failed:\n"
                    "${_otool_err}\n"
                    "Falling back to FetchContent build."
                )
                set(KAIROS_OTEL_SYSTEM_DEPS_OK FALSE PARENT_SCOPE)
                return()
            endif()
        endif()
    endif()

    # Windows: system OTel is not expected (disabled in build.ps1).
    # If somehow used, skip the check — static linking is typical on Windows.
endfunction()
