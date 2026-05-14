# CMakeLists.txt for zfs_remoted - ZFS Remote Block Device Daemon
#
# This is a standalone Windows user-mode application. It does NOT link to
# any ZFS libraries and does NOT include ZFS kernel headers.
#
# Source files:
#   zfs_remoted.c  - main: arg parsing, network, RPC dispatch
#   backend_file.c - file backend (serves a raw image file)
#   backend_disk.c - disk backend (serves a physical Windows disk)

add_executable(zfs_remoted
  zfs_remoted.c
  backend_file.c
  backend_disk.c
)

# The parent cmake scope adds ZFS kernel headers globally via
# include_directories(), which shadows MSVC's system <string.h> (ZFS's
# wrapper uses #include_next, unsupported by MSVC). We reset this target's
# includes to only the Windows SDK user-mode paths.
set_target_properties(zfs_remoted PROPERTIES INCLUDE_DIRECTORIES "")

# Re-add Windows SDK UM paths so winsock2.h, winioctl.h etc. are found.
# WDK_VERSION and WDK_ROOT are already set by the parent build system.
target_include_directories(zfs_remoted PRIVATE
  "${WDK_ROOT}/Include/${WDK_VERSION}/um"
  "${WDK_ROOT}/Include/${WDK_VERSION}/shared"
  "${WDK_ROOT}/Include/${WDK_VERSION}/ucrt"
  "${CMAKE_CURRENT_SOURCE_DIR}"     # for "zfs_remoted.h"
)

install(TARGETS zfs_remoted RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
