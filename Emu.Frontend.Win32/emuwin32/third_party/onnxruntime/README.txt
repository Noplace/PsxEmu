ONNX Runtime 1.24.4's C API headers, as they come in the Microsoft.ML.OnnxRuntime.DirectML NuGet
package's build\native\include\ folder, under the MIT licence beside them (LICENSE.txt) - so the
emulator builds with the AI texture upscaler whether or not the runtime has been fetched. Only the
C API is used: onnxruntime_c_api.h, the execution-provider header it includes, and the DirectML
provider's dml_provider_factory.h (whose DirectML.h is the Windows SDK's).

Only the headers are here. onnxruntime.dll, onnxruntime_providers_shared.dll (MIT) and DirectML.dll
(Microsoft's DirectML redistributable licence, which allows shipping it inside a Windows
application or game) are fetched into Temp\onnxruntime\ by
..\..\upscale\texture\fetch_onnxruntime.ps1, never committed, and copied beside the tools by
build_tools.bat. See Docs/Texture-Upscaling-Plan.md, phase 0.
