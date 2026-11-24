# Third-party notices

This project is licensed under GPL-3.0-or-later. It uses or includes the following third-party components.

| Component | Use | License |
|---|---|---|
| [Eden](https://git.eden-emu.dev/eden-emu/eden) | Shader recompiler (`src/shader_recompiler`, `src/common`) at commit 060026dc, kept with this project's changes in `runtime/eden` (see its README and LICENSE.txt) | GPL-3.0-or-later |
| [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk) (via Eden) | Frame-generation integration in `runtime/src/nnsdk/nvn/lsfg/` (loads the user's own `Lossless.dll`; no Lossless Scaling code or shaders are included) | GPL-3.0-or-later |
| [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | Upscaling shaders (`runtime/src/nnsdk/nvn/shaders/`, compiled into `spirv.h`) | MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | SPIR-V optimizer, built from the copy shipped in the Android NDK | Apache-2.0 (see `runtime/recompiler/LICENSE.SPIRV-Tools`) |
| [sirit v1.0.5](https://github.com/eden-emulator/sirit/tree/v1.0.5) | SPIR-V emitter used by the shader recompiler | Its source files carry BSD-3-Clause notices. The fork also ships [LICENSE](https://github.com/eden-emulator/sirit/blob/v1.0.5/LICENSE) and [LICENSE.txt](https://github.com/eden-emulator/sirit/blob/v1.0.5/LICENSE.txt). |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) | SPIR-V grammar headers | MIT-style (Khronos) |
| [{fmt}](https://github.com/fmtlib/fmt) | Formatting library used by Eden's code | MIT |
| [Boost](https://www.boost.org/) (headers) | Used by Eden's shader recompiler | BSL-1.0 |
| Android NDK `native_app_glue` | NativeActivity glue | Apache-2.0 |
| [yun-wulian/mhrise-recomp](https://github.com/yun-wulian/mhrise-recomp) | The Lua runtime for mods, PC mod import, the Chinese menus and the NVN/Vulkan resource fixes (merged with its commit history) | GPL-3.0-or-later |
| [Lua](https://www.lua.org/license.html) 5.4.9 | Script runtime for mods | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | MOD UI and HUD | MIT |
| [FreeType](https://freetype.org/license.html) 2.13.3 | Font rendering for the MOD UI | FreeType Project License (FTL) |
| [JSON for Modern C++](https://github.com/nlohmann/json) 3.11.3 | Lua mod configuration and data | MIT |
| [FFmpeg](https://ffmpeg.org/legal.html) 9.0.2 | Optional MP4 demuxer, configured by `tools/build_ffmpeg.sh` | LGPL-2.1-or-later in this configuration |
| [libarchive-android](https://github.com/zhanghai/libarchive-android) | Reading mod archives and compressed original texture headers on Android | Apache-2.0 bindings; libarchive and bundled codec notices in `android/assets/licenses/mod-archives.txt` |
| [libadrenotools](https://github.com/bylaws/libadrenotools) and liblinkernsbypass | Loading the user's own Adreno driver (`runtime/external/libadrenotools`, unmodified) | BSD-2-Clause, Copyright (c) 2021 Billy Laws (text in its `LICENSE`) |
| [BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid) | The approach of `astc::from_bc1` (BC1 blocks turned into ASTC blocks directly: its `runtime/nvn/bc1_astc.c`); the code here is written from the ASTC specification | MIT, Copyright (c) 2026 Peiikai |
| [Arm ASTC Encoder (astcenc)](https://github.com/ARM-software/astc-encoder) 5.7.0 | Re-encoding BC textures as ASTC on GPUs without BC (`runtime/external/astcenc`, library sources only, unmodified) | Apache-2.0, Copyright 2020-2026 Arm Limited (text in its `LICENSE.txt`) |
| [volk](https://github.com/zeux/volk) | Vulkan function loader, so the driver can be the system's or the user's (`runtime/external/volk`, unmodified) | MIT, Copyright (c) 2018-2026 Arseny Kapoulkine (text in its `LICENSE.md`) |
| [mhrise-save-converter](https://github.com/jinghaihan/mhrise-save-converter) | In `SaveData.java` and `SteamDefaults.java`: which fields belong to the platform when a save is filled, the PC-only data and root order a Steam save gets without a Steam save to fill, the album groups and telling the Citrus curve by its block check; written again for the port's save tree | MIT, Copyright (c) jinghaihan |
| [ree-save-editor](https://github.com/kvasszn/ree-save-editor) | The save formats (DSSS, Citrus) that `SaveData.java` reads and writes, and the 128 Citrus curves in `android/assets/citrus_curves.bin` (the game's own constants, as that project lists them); the code here is written from the format notes and checked against real saves | GPL-3.0 |

Lossless Scaling and its `Lossless.dll` are not distributed here; the DLL must come from the user's own purchase.

This project is independently maintained by the community. It has no affiliation or partnership with Capcom or the official development or publishing teams for its games. They do not sponsor or endorse this project. Rights to Monster Hunter Rise, its DLC and associated trademarks belong to their respective owners.

This repository does not contain or distribute the original game's source code, executables, ROM images, game asset packages, Switch decryption keys or player saves. Players must extract the game program (ExeFS) and assets (RomFS) from their own legitimately obtained Nintendo Switch copy.

AMD FidelityFX FSR 1 license (MIT):

```
Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```
