# Agent Notes

- Local default builds through `build.ps1` should produce the DirectML variant by passing `-DUSE_DIRECTML=ON` and `-DUSE_BUNDLED_DIRECTML_RUNTIME=ON`.
- Keep CI unaffected: workflow builds invoke `cmake` directly with explicit provider flags, so do not change CI provider defaults unless requested.
- If a local CPU build is needed, override the helper default with `./build.ps1 -CMakeArgs @('-DUSE_DIRECTML=OFF', '-DUSE_BUNDLED_DIRECTML_RUNTIME=OFF')`.
