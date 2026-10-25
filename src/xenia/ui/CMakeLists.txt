add_library(xenia-ui STATIC)
xe_platform_sources(xenia-ui ${CMAKE_CURRENT_SOURCE_DIR})
# Exclude demo and windowed_app_main files
file(GLOB _ui_exclude   "${CMAKE_CURRENT_SOURCE_DIR}/*_demo.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/windowed_app_main_*.cc"
)
get_target_property(_ui_srcs xenia-ui SOURCES)
if(_ui_exclude AND _ui_srcs)
  list(REMOVE_ITEM _ui_srcs ${_ui_exclude})
  set_target_properties(xenia-ui PROPERTIES SOURCES "${_ui_srcs}")
endif()
target_link_libraries(xenia-ui PUBLIC xenia-base)
if(WIN32)
  target_link_libraries(xenia-ui PRIVATE dwmapi dxgi winmm)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(xenia-ui PRIVATE xcb X11 X11-xcb fontconfig ${GTK3_LIBRARIES})
endif()
xe_target_defaults(xenia-ui)

if(WIN32)
  # DLSS SR backend (Milestone 3-B). Windows-only (D3D12). Depends only on
  # xenia-base; the SDK headers are referenced by path, not copied.
  add_subdirectory(dlss)
  # NVOF (NVIDIA Optical Flow) backend (Milestone 4-D). Windows-only (D3D12).
  # Depends only on xenia-base; the SDK headers are referenced by path, not
  # copied. Independent module - orthogonal to the DLSS SR backend.
  add_subdirectory(nvof)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Japanese glyphs for ImGui. fontconfig's CJK match is often a CFF or
  # variable font that stb_truetype cannot load (see LoadJapaneseFont).
  xe_embed_binary_assets(xenia-ui "${PROJECT_SOURCE_DIR}/assets/font" font)
endif()

if(XENIA_BUILD_TESTS)
  set(CMAKE_FOLDER "tests")
  add_subdirectory(testing)
endif()
