# CMakeLists.txt for zfs_remoted - ZFS Remote Block Device Daemon
# Windows-only TCP daemon that serves a raw image file as a block device.
#
# This is a standalone Windows user-mode application. It does NOT link to
# any ZFS libraries and does NOT include ZFS kernel headers.

add_executable(zfs_remoted
  zfs_remoted.c
)

# Clear inherited include directories from parent scope to avoid pulling
# in ZFS kernel headers (which cause include_next failures with MSVC).
set_target_properties(zfs_remoted PROPERTIES
  INCLUDE_DIRECTORIES ""
)

install(TARGETS zfs_remoted RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
