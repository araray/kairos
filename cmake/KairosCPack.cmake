# ╔════════════════════════════════════════════════════════════════════════════╗
# ║  cmake/KairosCPack.cmake — Binary packaging (DEB, RPM, ZIP)             ║
# ║  Spec reference: §28.7                                                  ║
# ╚════════════════════════════════════════════════════════════════════════════╝

# ── Generic CPack settings ─────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "kairos")
set(CPACK_PACKAGE_VENDOR "Kairos Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Unified orchestration daemon — scheduling, workflows, and filesystem monitoring")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_CONTACT "kairos@example.com")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/araray/kairos")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

# Multi-generator support.
set(CPACK_GENERATOR "DEB;RPM;ZIP")

# ── DEB-specific settings ─────────────────────────────────────────────────
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Kairos Team")
set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libsqlite3-0 (>= 3.35)")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
    "Unified orchestration daemon — scheduling, workflows, and filesystem monitoring\n"
    " Kairos merges scheduling (cron/interval), DAG-based workflow execution,\n"
    " and filesystem monitoring into a single daemon with SQLite persistence,\n"
    " structured logging, MCP agent interface, HTTP dashboard, and a\n"
    " comprehensive CLI.")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/araray/kairos")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

# Post-install/pre-remove scripts: create user, dirs, enable service.
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_SOURCE_DIR}/deploy/deb/postinst;${CMAKE_SOURCE_DIR}/deploy/deb/prerm")

# ── RPM-specific settings ─────────────────────────────────────────────────
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "System Environment/Daemons")
set(CPACK_RPM_PACKAGE_REQUIRES "sqlite >= 3.35")
set(CPACK_RPM_PACKAGE_DESCRIPTION
    "Unified orchestration daemon — scheduling, workflows, and filesystem monitoring")
set(CPACK_RPM_PACKAGE_URL "https://github.com/araray/kairos")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)

# RPM post-install script.
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
    "${CMAKE_SOURCE_DIR}/deploy/rpm/postinst.sh")
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE
    "${CMAKE_SOURCE_DIR}/deploy/rpm/prerm.sh")

# ── ZIP settings (Windows / generic) ──────────────────────────────────────
set(CPACK_ARCHIVE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

include(CPack)
