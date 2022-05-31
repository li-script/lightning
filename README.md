<p align="center">
  <a href="https://www.liscript.org/">
    <img width="256" heigth="256" src="https://liscript.org/logo.png">
  </a>

  <h1 align="center">Lightning Script</h1>

  <p align="center">
    <a href="https://github.com/li-script/lightning/actions/workflows/CMake.yml">
      <img alt="github-actions" src="https://img.shields.io/github/workflow/status/li-script/lightning/CMake?style=flat-square"/>
    </a>
    <a href="https://github.com/li-script/lightning/blob/master/LICENSE.md">
      <img alt="license" src="https://img.shields.io/github/license/li-script/lightning.svg?style=flat-square"/>
    </a>
  </p>
</p>

Lightning Script is a work-in-progress scripting language with a modern syntax, uncompromising performance, and first-class embedding support.

## Building

```
cmake -B build
cmake --build build
```

For the best performance, we recommend you compile with `-flto` for GCC/Clang and `/LTCG` for MSVC.
Modify `cmake.toml` to change the build settings.
