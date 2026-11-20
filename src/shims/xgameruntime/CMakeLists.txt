# xgameruntime.dll replacement (T-012). Output name must be exactly xgameruntime.dll:
# the GDK's static lib prefers a copy next to the game executable.
add_library(gtx_xgameruntime SHARED xgameruntime.cpp impl.cpp exports.def)
target_link_libraries(gtx_xgameruntime PRIVATE gtx_common)
target_include_directories(gtx_xgameruntime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(gtx_xgameruntime PROPERTIES OUTPUT_NAME xgameruntime)
