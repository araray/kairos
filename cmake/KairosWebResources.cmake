# ╔════════════════════════════════════════════════════════════════════════════╗
# ║  cmake/KairosWebResources.cmake — web asset embedding helper              ║
# ║                                                                          ║
# ║  Usage (from any CMakeLists that needs the embedded web assets):          ║
# ║                                                                          ║
# ║      include(KairosWebResources)                                         ║
# ║      kairos_embed_web_resources(gen_src)                                 ║
# ║      target_sources(<target> PRIVATE ${gen_src})                         ║
# ║                                                                          ║
# ║  Defines a build rule that regenerates <dir>/generated/web_resources.cpp  ║
# ║  whenever any file under web/ changes, and returns its path. Each         ║
# ║  consuming directory gets its own copy (custom commands are               ║
# ║  directory-scoped).                                                       ║
# ╚════════════════════════════════════════════════════════════════════════════╝

function(kairos_embed_web_resources out_var)
    set(web_dir "${CMAKE_SOURCE_DIR}/web")
    if(NOT EXISTS "${web_dir}")
        message(FATAL_ERROR "kairos_embed_web_resources: ${web_dir} does not exist")
    endif()

    file(GLOB_RECURSE web_files
        RELATIVE "${web_dir}"
        CONFIGURE_DEPENDS
        "${web_dir}/*")

    if(NOT web_files)
        message(FATAL_ERROR "kairos_embed_web_resources: no assets found under ${web_dir}")
    endif()

    list(SORT web_files)
    list(LENGTH web_files web_count)

    set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(manifest "${gen_dir}/web_manifest.cmake")
    set(output "${gen_dir}/web_resources.cpp")

    # Write the manifest (sorted relative paths).
    set(manifest_content "set(WEB_FILES\n")
    foreach(f IN LISTS web_files)
        string(APPEND manifest_content "    \"${f}\"\n")
    endforeach()
    string(APPEND manifest_content ")\nset(WEB_COUNT ${web_count})\n")
    file(WRITE "${manifest}" "${manifest_content}")

    # Absolute dependencies for the custom command.
    set(absolute_deps "")
    foreach(f IN LISTS web_files)
        list(APPEND absolute_deps "${web_dir}/${f}")
    endforeach()

    add_custom_command(
        OUTPUT "${output}"
        COMMAND ${CMAKE_COMMAND}
                -DWEB_DIR=${web_dir}
                -DMANIFEST=${manifest}
                -DOUTPUT=${output}
                -P ${CMAKE_SOURCE_DIR}/cmake/GenerateWebResources.cmake
        DEPENDS ${absolute_deps}
                ${CMAKE_SOURCE_DIR}/cmake/GenerateWebResources.cmake
        COMMENT "Generating embedded web resources (${web_count} assets)"
        VERBATIM)

    set(${out_var} "${output}" PARENT_SCOPE)
endfunction()
