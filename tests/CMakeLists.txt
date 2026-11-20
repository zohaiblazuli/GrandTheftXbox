add_executable(gtx_common_test common_test.cpp)
target_link_libraries(gtx_common_test PRIVATE gtx_common)
add_test(NAME gtx_common_test COMMAND gtx_common_test)

add_executable(xgameruntime_shim_test xgameruntime_shim_test.cpp)
add_dependencies(xgameruntime_shim_test gtx_xgameruntime)
add_test(NAME xgameruntime_shim_test COMMAND xgameruntime_shim_test
         WORKING_DIRECTORY $<TARGET_FILE_DIR:gtx_xgameruntime>)

# Optional: end-to-end through Microsoft's real static lib (public PC GDK, see docs TOOLS).
set(GTX_GDK_WINDOWS_DIR "" CACHE PATH "Microsoft.GDK.Windows .../native/<ver>/windows")
if(GTX_GDK_WINDOWS_DIR AND EXISTS "${GTX_GDK_WINDOWS_DIR}/lib/x64/xgameruntime.lib")
  add_executable(xgameruntime_gdk_e2e xgameruntime_gdk_e2e.cpp)
  target_include_directories(xgameruntime_gdk_e2e PRIVATE "${GTX_GDK_WINDOWS_DIR}/include")
  target_link_libraries(xgameruntime_gdk_e2e PRIVATE
                        "${GTX_GDK_WINDOWS_DIR}/lib/x64/xgameruntime.lib" advapi32)
  add_dependencies(xgameruntime_gdk_e2e gtx_xgameruntime)
  add_test(NAME xgameruntime_gdk_e2e COMMAND xgameruntime_gdk_e2e)

  # Same program without self-preload: gtx-launch must inject the shim.
  add_executable(xgameruntime_gdk_probe xgameruntime_gdk_e2e.cpp)
  target_compile_definitions(xgameruntime_gdk_probe PRIVATE GTX_NO_SELF_PRELOAD)
  target_include_directories(xgameruntime_gdk_probe PRIVATE "${GTX_GDK_WINDOWS_DIR}/include")
  target_link_libraries(xgameruntime_gdk_probe PRIVATE
                        "${GTX_GDK_WINDOWS_DIR}/lib/x64/xgameruntime.lib" advapi32)
  add_dependencies(xgameruntime_gdk_probe gtx_xgameruntime gtx_launch)
  add_test(NAME gtx_launch_injects_shim
           COMMAND gtx_launch --wait-ms 30000 $<TARGET_FILE:xgameruntime_gdk_probe>)

  function(gtx_gdk_test name src)
    add_executable(${name} ${src})
    target_include_directories(${name} PRIVATE "${GTX_GDK_WINDOWS_DIR}/include")
    target_link_libraries(${name} PRIVATE "${GTX_GDK_WINDOWS_DIR}/lib/x64/xgameruntime.lib" advapi32)
    add_dependencies(${name} gtx_xgameruntime)
  endfunction()

  gtx_gdk_test(xtask_gdk_test xtask_gdk_test.cpp)
  add_test(NAME xtask_gdk_test COMMAND xtask_gdk_test)
  # Same assertions against Microsoft's installed runtime (skips itself if absent).
  gtx_gdk_test(xtask_gdk_reference xtask_gdk_test.cpp)
  target_compile_definitions(xtask_gdk_reference PRIVATE GTX_REFERENCE_RUNTIME)
  add_test(NAME xtask_gdk_reference COMMAND xtask_gdk_reference)
else()
  message(STATUS "GTX_GDK_WINDOWS_DIR not set: skipping GDK tests")
endif()
