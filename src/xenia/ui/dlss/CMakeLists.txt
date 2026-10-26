# NVIDIA graphics-enhancement backends (DLSS SR, DLSS FG, NVOF) and the shared
# NGX core owner.
#
# This directory groups the independent NVIDIA feature backends and the shared
# NGX core owner under the dlss/ namespace. Each backend lives in its own
# subdirectory and is the ONLY place in the codebase that depends on its NVIDIA
# SDK surface (license isolation):
#   - ngx/  -> shared NGX core owner (Milestone 5-B), target xenia-ngx
#   - sr/   -> DLSS super-sampling (Milestone 3-B), target xenia-dlss
#   - fg/   -> DLSS frame generation / DLSSG (Milestone 5-B), target xenia-fg
#   - nvof/ -> NVIDIA optical flow (Milestone 4-D), target xenia-nvof
#
# The NGX core owner is added first so the SR and FG backends can link against
# it. The SR and FG backends acquire/release the shared NGX core through the
# core owner. NVOF is independent (it does not use the NGX core).
#
# If a backend's SDK license is ultimately ruled incompatible with distribution,
# that backend's subdirectory can be deleted with zero changes to the rest of
# the codebase (the other backends are independent).
add_subdirectory(ngx)
add_subdirectory(sr)
add_subdirectory(fg)
add_subdirectory(nvof)
