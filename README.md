<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/logo/doriax_logo_transparent.png">
    <source media="(prefers-color-scheme: light)" srcset="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/logo/doriax_logo_dark.png">
    <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/logo/doriax_logo_dark.png" alt="Doriax Engine" width="300">
  </picture>
</p>

<p align="center">
  A free, open-source 2D/3D game engine and integrated editor for building cross-platform games and interactive projects with Lua or C++.
</p>

<p align="center">
  <strong>Doriax Engine</strong> is a lightweight, ECS-based game engine built around data-oriented design, with an integrated workflow for visual editing, shader authoring, and project export.
</p>

<p align="center">
  <a href="https://github.com/doriaxengine/doriax/actions/workflows/cmake.yml"><img src="https://github.com/doriaxengine/doriax/actions/workflows/cmake.yml/badge.svg?branch=main" alt="Editor Desktop"></a>
  <a href="https://github.com/doriaxengine/doriax/actions/workflows/engine-cmake.yaml"><img src="https://github.com/doriaxengine/doriax/actions/workflows/engine-cmake.yaml/badge.svg?branch=main" alt="Engine Desktop"></a>
  <a href="https://github.com/doriaxengine/doriax/actions/workflows/engine-android.yml"><img src="https://github.com/doriaxengine/doriax/actions/workflows/engine-android.yml/badge.svg?branch=main" alt="Engine Android"></a>
  <a href="https://github.com/doriaxengine/doriax/actions/workflows/engine-emscripten.yaml"><img src="https://github.com/doriaxengine/doriax/actions/workflows/engine-emscripten.yaml/badge.svg?branch=main" alt="Engine Emscripten"></a>
  <a href="https://github.com/doriaxengine/doriax/actions/workflows/engine-xcode.yaml"><img src="https://github.com/doriaxengine/doriax/actions/workflows/engine-xcode.yaml/badge.svg?branch=main" alt="Engine iOS/macOS"></a>
  <a href="https://discord.gg/yXXDyJf3gT"><img src="https://img.shields.io/discord/1356958061880934480?label=Discord&logo=discord&style=flat&color=5865F2" alt="Join our Discord"></a>
</p>

<p align="center">
  <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/editor-lighting.png" alt="Doriax Engine Editor">
</p>

<p align="center">
  <a href="https://www.youtube.com/watch?v=3eqhaAZBNss">Introducing Doriax Engine - see feature highlights</a>
</p>

## Overview

Doriax Engine combines a lightweight runtime with a modern desktop editor for creating 2D and 3D games. The project keeps its ECS and data-oriented foundation, but now extends far beyond a runtime API: author scenes visually, script in Lua or C++, generate shader data, export projects, and build for multiple platforms from a single environment.

## Why Doriax

- Integrated editor for 2D and 3D scene creation
- Built-in AI assistant that creates entities and scripts from natural language
- Project generation, build orchestration, and export tooling
- Shader compilation and cross-platform shader translation for exported projects
- Lua for fast iteration and C++ for native performance
- ECS-based, data-oriented runtime designed for efficiency
- Cross-platform deployment with OpenGL, Vulkan, Metal, and DirectX backends

## Editor Workflow

- Scene hierarchy, inspector, and resource management
- 2D tools for sprites, tilemaps, and sprite slicing
- 3D scene editing with cameras, lighting, models, and play mode
- Animation tools, timeline editing, and bone workflows
- Integrated code editor and scripting workflow for Lua and C++
- AI assistant: a built-in chat that works as an agent inside the editor — describe a task in plain language and it creates entities, writes and edits Lua or C++ scripts, and builds your project, using configurable AI models
- Custom shaders: fork and edit the built-in Mesh, UI, Points, Lines, and Sky shaders per component, with live recompile in the viewport
- Shader-aware export pipeline that prepares scenes, assets, scripts, engine files, and compiled shaders

<p align="center">
  <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/readme/editor-office-scene.png" alt="Doriax Engine office scene" width="48%">
  <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/editor-2d-sprite.png" alt="Doriax Engine 2D sprite and tilemap editor" width="48%">
</p>
<p align="center">
  <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/readme/editor-code.png" alt="Doriax Engine integrated code editor" width="48%">
  <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/readme/runtime-first-ui-scene.png" alt="Doriax Engine UI tools" width="48%">
</p>
<p align="center">
  <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/readme/editor-ai-chat.png" alt="Doriax Engine AI assistant creating entities and scripts" width="48%">
  <img src="https://raw.githubusercontent.com/doriaxengine/doriax-site/main/screenshots/editor-bones.png" alt="Doriax Engine bone animation editor" width="48%">
</p>

## Engine Features

- Shared ECS runtime for 2D and 3D scenes, with scene layers and serialization
- Sprites, tilemaps, and polygons, plus 2D lights, normal maps, and occluder shadows
- GLTF and OBJ models with skeletal animation, morph targets, and mesh instancing
- PBR materials with dynamic lights and cascaded shadows, sky-driven IBL, SSAO, fog, and planar mirrors
- Keyframe tracks, runtime actions with easing, and sprite sheet animation
- Particle systems, heightmap terrain with clipmap LOD, and a UI toolkit with anchors and widgets
- Integrated 2D and 3D physics via Box2D and Jolt Physics (bodies, shapes, and joints)
- Spatial 3D audio, texture and shader pools, and multithreaded async resource loading

## Platforms

| Area | Support |
| --- | --- |
| Editor downloads | Windows, Linux, macOS |
| Project targets | Windows, Linux, macOS, Android, iOS, HTML5 |
| Graphics APIs | OpenGL, Vulkan, Metal, DirectX |
| Scripting | Lua, C++ |

## Getting Started

Download a prebuilt editor from [doriax.org](https://doriax.org/#download) or build from source:

```bash
git clone https://github.com/doriaxengine/doriax.git
cd doriax
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The root project builds `doriax-editor`. On single-config generators the executable is typically created under `build/`. On multi-config generators such as Visual Studio, look under the configuration subdirectory. Platform-specific setup is still being refreshed under the Doriax name.

## Documentation and Community

- [Website](https://doriax.org)
- [Discord](https://discord.gg/yXXDyJf3gT)

## Repository Layout

- `editor/` - desktop editor, windows, tools, project generation, and export flow
- `engine/` - runtime engine, platform layers, rendering, ECS, and project templates
- `shadercompiler/` - shader compilation and translation tools
- `libs/` - bundled third-party dependencies

## Transition from Supernova

Doriax Engine is the next phase of Supernova Engine. Version `0.5.5` was the last release of the legacy Supernova engine. Some internal folders, and older external references may still mention the previous name while the rebranding and documentation refresh continue.

## License

Doriax Engine is released under the MIT License and is free for personal and commercial use. Third-party libraries included in the repository keep their own licenses.
