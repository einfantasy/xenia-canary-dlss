# DLSS SR backend (Milestone 3-B).
#
# This module is the ONLY place in the codebase that depends on the NVIDIA NGX
# SDK (license isolation). The SDK headers are referenced by path (NOT copied
# into the Xenia source tree), and the SDK runtime DLLs are resolved at runtime
# by the presenter (NOT bundled with the build). If the SDK license is
# ultimately ruled incompatible with distribution, this entire module can be
# deleted with zero changes to the rest of the codebase.
#
# The module depends only on xenia-base; it does not link against the D3D12
# provider utilities (avoiding a circular dependency with the presenter target).

# The DLSS SR backend (Milestone 5-B) acquires/releases the shared NGX core
# through the core owner (xenia-ngx) instead of calling Init_with_ProjectID /
# Shutdown1 / AllocateParameters / DestroyParameters itself. The core owner is
# the ONLY place that calls those core lifecycle entry points; the SR backend
# keeps its feature-level calls (GetFeatureRequirements / CreateFeature /
# EvaluateFeature / ReleaseFeature).
add_library(xenia-dlss STATIC)
xe_platform_sources(xenia-dlss ${CMAKE_CURRENT_SOURCE_DIR})

# The NVIDIA SDK headers are not part of the Xenia source tree. They are
# referenced from the local SDK install. Override with -DXENIA_DLSS_SDK_DIR.
if(NOT DEFINED XENIA_DLSS_SDK_DIR)
  set(XENIA_DLSS_SDK_DIR "C:/SDKs/NVIDIA/DLSS/DLSS-310.9.1")
endif()
target_include_directories(xenia-dlss PRIVATE
  "${XENIA_DLSS_SDK_DIR}/include"
  # rapidjson (header-only, vendored in third_party) parses the ngx-local.json
  # config. The global include is only third_party (which resolves to
  # third_party/rapidjson/, not third_party/rapidjson/include/), so the
  # include subdir must be added explicitly for this target.
  "${PROJECT_SOURCE_DIR}/third_party/rapidjson/include")

# The runtime DLLs (nvngxruntime.dll, nvngx_dlss.dll) are resolved at runtime
# by D3D12Presenter::GetDLSSRuntimeDirectories (relative to the executable).
# They are NOT bundled with the build or copied into the source tree.
target_link_libraries(xenia-dlss PUBLIC xenia-base
  # The static NGX core resolves the NVSDK_NGX_* entry points at link time.
  # The "_d" variant is the dynamic-CRT (/MD) build, matching Xenia's /MD
  # runtime for every configuration (the "_s" variant is /MT and would mix
  # two CRTs). The static core statically imports NO NVIDIA DLL: the feature
  # DLL (nvngx_dlss.dll) is loaded at CreateFeature time from the PathList
  # directories, so the executable starts without any NVIDIA runtime present
  # (the "off path has zero DLL loads" contract).
  "${XENIA_DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib"
  # The shared NGX core owner (Init_with_ProjectID / Shutdown1 /
  # AllocateParameters / DestroyParameters) that the SR backend acquires
  # (Milestone 5-B). The core owner is the ONLY place that calls those core
  # lifecycle entry points.
  xenia-ngx)
xe_target_defaults(xenia-dlss)
