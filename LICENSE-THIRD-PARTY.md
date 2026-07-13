# Third-Party Licenses — `wmr` release binaries

Every `wmr` release binary is a self-contained package that ships the FDnCNN AI
denoise (NCNN + volk, an embedded model) and MI-GAN inpaint (a bundled model).
NCNN, volk, FDnCNN, OpenCV (incl. the opencv_contrib `xphoto` module, still
linked though FSR is now removed — pending a cleanup), and — on Linux/Windows —
ONNX Runtime are linked into the package; their licenses are reproduced below so
they travel with every download.

**As of v1.10.0, macOS (arm64 + Intel) runs MI-GAN via Apple's CoreML framework**
(a system framework linked at build time, not a vendored library — no third-party
component is added to the macOS packages; the MI-GAN model ships as a 14 MB
`.mlpackage`). ONNX Runtime is no longer shipped on macOS. Linux/Windows continue
to use ONNX Runtime (MIT) as before.

The macOS arm64 package additionally bundles the Vulkan loader (`libvulkan`) and
MoltenVK (`libMoltenVK`) for the AI-denoise GPU path. The Linux and Windows
packages co-locate `libonnxruntime.so.1` / `onnxruntime.dll` and the MI-GAN
(ONNX) model. MoltenVK ships its own license inside its dylib.

---

## NCNN — BSD 3-Clause License

Source: <https://github.com/Tencent/ncnn>

Copyright (c) 2017 THL A29 Limited, a Tencent company. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

---

## volk — MIT License

Source: <https://github.com/zeux/volk>

Copyright (c) 2018-2024 Arseny Kapoulkine

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

## KAIR / FDnCNN — MIT License

Source: <https://github.com/csjcai/KAIR>

The FDnCNN (Flexible DnCNN) denoising model is © its authors and distributed
under the MIT License as part of the KAIR project.

Copyright (c) the KAIR authors.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

## OpenCV / opencv_contrib / xphoto — Apache License 2.0

Source: <https://github.com/opencv/opencv> · <https://github.com/opencv/opencv_contrib>

The `wmr` binary links OpenCV core modules and the opencv_contrib `xphoto`
module (FSR inpaint for NotebookLM intricate backgrounds). OpenCV and
opencv_contrib are licensed under the Apache License, Version 2.0. A complete
copy of the license is at <https://www.apache.org/licenses/LICENSE-2.0>.

Copyright © 2000-2025 OpenCV team.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
these files except in compliance with the License. You may obtain a copy of the
License at <https://www.apache.org/licenses/LICENSE-2.0>. Unless required by
applicable law or agreed to in writing, software distributed under the License is
distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
either express or implied. See the License for the specific language governing
permissions and limitations under the License.

---

## Model conversion credit

The FDnCNN model was converted to the NCNN format used here by
[allenk/GeminiWatermarkTool](https://github.com/allenk/GeminiWatermarkTool)
(MIT). The committed NCNN model headers (`assets/model_core.{mem.h,id.h}`)
were captured from a one-time build of that project, which runs `ncnn2mem` to
emit the embedded `.mem.h`/`.id.h` headers from the FDnCNN `.param`/`.bin`.
The model weights are upstream's converted bytes; no re-conversion was
performed for this project.

---

## ONNX Runtime — MIT License

Source: <https://github.com/microsoft/onnxruntime>

The `wmr` binary loads ONNX Runtime (the official prebuilt v1.27.1 shared
library, fetched at CMake configure time) to run the LaMa inpainter. ONNX
Runtime is distributed under the MIT License.

Copyright (c) Microsoft Corporation.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

## MI-GAN inpaint model — MIT License

Source: <https://github.com/Picsart-AI-Research/MI-GAN> · ONNX: <https://huggingface.co/andraniksargsyan/migan>

`wmr` ships `assets/migan_pipeline_v2.onnx` — the MI-GAN ("A Simple Baseline for
Image Inpainting on Mobile Devices", ICCV 2023, Picsart AI Research) model,
co-located with every MI-GAN-enabled release binary (macOS arm64, Linux,
Windows). It is the default NotebookLM intricate-scene inpainter (replaces the
earlier FSR/LaMa).

MI-GAN's **code and weights** are licensed under the **MIT License** — clean for
redistribution (unlike the LaMa weights it replaces, which were Places2-pretrained
under a gray-area license).

Copyright (c) Picsart AI Research.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
