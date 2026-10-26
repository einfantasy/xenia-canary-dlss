# DLSS Frame Generation (FG / DLSSG) backend (Milestone 5-B).
#
# This module is the ONLY place in the codebase that depends on the NVIDIA FG
# (DLSSG) SDK surface (license isolation). In Milestone 5-B the backend is a
# stub (no real SDK call, no GPU work), so this module includes NO NVIDIA SDK
# header and links NO NVIDIA library. The FG SDK headers (nvsdk_ngx_defs_dlssg.h,
# nvsdk_ngx_params_dlssg.h, nvsdk_ngx_helpers_dlssg_d3d.h) are referenced by
# path (NOT copied into the Xenia source tree) and included only in the .cc
# starting in Milestone 5-C. The FG runtime DLL (nvngx_dlssg.dll, staged in
# runtime/experimental) is resolved at runtime by the static NGX core (NOT
# bundled with the build). If the FG license is ultimately ruled incompatible
# with distribution, this entire module can be deleted with zero changes to the
# rest of the codebase.
#
# The module depends on xenia-base (for logging) and xenia-ngx (the shared NGX
# core owner that the FG backend acquires/releases; the core owner is the ONLY
# place that calls Init_with_ProjectID / Shutdown1 / AllocateParameters /
# DestroyParameters). The FG backend keeps its feature-level calls
# (GetFeatureRequirements / CreateFeature / EvaluateFeature / ReleaseFeature).
# It does NOT link against the D3D12 provider utilities (avoiding a circular
# dependency with the presenter target).
add_library(xenia-fg STATIC)
xe_platform_sources(xenia-fg ${CMAKE_CURRENT_SOURCE_DIR})

# The NVIDIA SDK headers are not part of the Xenia source tree. They are
# referenced from the local SDK install (the FG-specific headers are confined
# to this module). Override with -DXENIA_FG_SDK_DIR.
if(NOT DEFINED XENIA_FG_SDK_DIR)
  set(XENIA_FG_SDK_DIR "C:/SDKs/NVIDIA/DLSS/DLSS-310.9.1")
endif()
target_include_directories(xenia-fg PRIVATE
  "${XENIA_FG_SDK_DIR}/include"
  # rapidjson (header-only, vendored in third_party) parses the ngx-local.json
  # config. The global include is only third_party (which resolves to
  # third_party/rapidjson/, not third_party/rapidjson/include/), so the
  # include subdir must be added explicitly for this target.
  "${PROJECT_SOURCE_DIR}/third_party/rapidjson/include")

# The runtime DLLs (nvngxruntime.dll, nvngx_dlssg.dll) are resolved at runtime
# by D3D12Presenter::GetDLSSRuntimeDirectories (relative to the executable).
# They are NOT bundled with the build or copied into the source tree.
target_link_libraries(xenia-fg PUBLIC xenia-base
  # The static NGX core resolves the NVSDK_NGX_* entry points at link time.
  # The "_d" variant is the dynamic-CRT (/MD) build, matching Xenia's /MD
  # runtime for every configuration. The static core statically imports NO
  # NVIDIA DLL: the feature DLL (nvngx_dlssg.dll) is loaded at CreateFeature
  # time from the PathList directories, so the executable starts without any
  # NVIDIA runtime present (the "off path has zero DLL loads" contract).
  "${XENIA_FG_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib"
  # The shared NGX core owner (Init_with_ProjectID / Shutdown1 /
  # AllocateParameters / DestroyParameters) that the FG backend acquires.
  xenia-ngx)
xe_target_defaults(xenia-fg)
