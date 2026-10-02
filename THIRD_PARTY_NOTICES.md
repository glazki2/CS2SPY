# Third-party notices

The module uses the following third-party software. Each project remains under its own license and copyright.

- [Admin System](https://github.com/Pisex/cs2-admin_system) and [Utils](https://github.com/Pisex/cs2-menus): their public API headers are included as `src/include/admin.h`, `src/include/utils.h` and `src/include/players.h` to call these plugins.
- [Metamod:Source](https://github.com/alliedmodders/metamod-source), downloaded at the commits pinned in `build-linux.sh`. The AMBuild scripts (`AMBuildScript`, `configure.py`, `PackageScript`) follow its sample plugin, and `hl2sdk-manifests` is its [hl2sdk-manifests](https://github.com/alliedmodders/hl2sdk-manifests).
- [Source 2 SDK](https://github.com/alliedmodders/hl2sdk), CS2 branch, downloaded at the commit pinned in `build-linux.sh`. Individual SDK files retain their Valve and contributor notices.
- [AMBuild](https://github.com/alliedmodders/ambuild), the build tool. It is licensed under the BSD 3-Clause License.
- [Protocol Buffers](https://github.com/protocolbuffers/protobuf), supplied by the Source 2 SDK and generated during the build. It is licensed under the BSD 3-Clause License.
