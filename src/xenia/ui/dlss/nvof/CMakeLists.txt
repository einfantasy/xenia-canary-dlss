# NVOF (NVIDIA Optical Flow) backend (Milestone 4-D).
#
# This module is the ONLY place in the codebase that depends on the NVIDIA
# Optical Flow SDK (license isolation). The SDK headers are referenced by path
# (NOT copied into the Xenia source tree), and the SDK runtime DLL
# (nvofapi64.dll, shipped with the NVIDIA driver) is resolved at runtime by
# the backend (NOT bundled with the build). If the NVOF license is ultimately
# ruled incompatible with distribution, this entire module can be deleted with
# zero changes to the rest of the codebase.
#
# The module depends only on xenia-base; it does not link against the D3D12
# provider utilities (avoiding a circular dependency with the presenter target).
add_library(xenia-nvof STATIC)
xe_platform_sources(xenia-nvof ${CMAKE_CURRENT_SOURCE_DIR})

# The NVIDIA Optical Flow SDK headers are not part of the Xenia source tree.
# They are referenced from the local SDK install. Override with
# -DXENIA_NVOF_SDK_DIR.
if(NOT DEFINED XENIA_NVOF_SDK_DIR)
  set(XENIA_NVOF_SDK_DIR "C:/SDKs/NVIDIA/Optical_Flow_SDK_5.0.7")
endif()
target_include_directories(xenia-nvof PRIVATE
  "${XENIA_NVOF_SDK_DIR}/NvOFInterface")

# The runtime DLL (nvofapi64.dll) is resolved at runtime by the backend via
# LoadLibraryExW (relative to the executable, or System32). It is NOT bundled
# with the build or copied into the source tree, and the module links NO
# NVIDIA import library (the DLL is loaded dynamically at Initialize time, so
# the executable starts without any NVIDIA runtime present - the "off path has
# zero DLL loads" contract).
# The NVOF module's public interface uses D3D12 types and the module calls
# D3D12 entry points (D3D12SerializeRootSignature) directly, so d3d12 is a
# public link dependency (consumers such as the test suite also call
# D3D12CreateDevice).
target_link_libraries(xenia-nvof PUBLIC xenia-base d3d12)
xe_target_defaults(xenia-nvof)
