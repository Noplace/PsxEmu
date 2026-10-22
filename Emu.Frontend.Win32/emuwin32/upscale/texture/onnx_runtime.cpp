/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#include "emuwin32/upscale/texture/onnx_runtime.h"

#include <DirectML.h>
#include <DirectXPackedVector.h>
#include "emuwin32/third_party/onnxruntime/dml_provider_factory.h"

#include <softpub.h>
#include <wintrust.h>

#include <mutex>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace emulation::win32 {

    namespace {

        bool Exists(const std::wstring& path) {
            return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        std::string Narrow(const std::wstring& text) {
            std::string out;
            for (wchar_t c : text)
                out += c < 128 ? static_cast<char>(c) : '?';
            return out;
        }

        typedef HRESULT(WINAPI* CreateDmlDeviceFunction)(ID3D12Device*, DML_CREATE_DEVICE_FLAGS,
                                                         REFIID, void**);
        typedef const OrtApiBase*(ORT_API_CALL* GetApiBaseFunction)();

        const char* kFiles[] = { "DirectML.dll", "onnxruntime_providers_shared.dll",
                                 "onnxruntime.dll" };

    }   // namespace

    OnnxRuntime& OnnxRuntime::Get() {
        static OnnxRuntime instance;
        return instance;
    }

    bool SignedByMicrosoft(const std::wstring& path) {
        // ONNX Runtime is signed as "Microsoft Corporation" and DirectML as "Microsoft Windows
        // Publisher": the organisation is the same.
        return SignedByOrganisation(path, "Microsoft Corporation");
    }

    bool SignedByOrganisation(const std::wstring& path, const char* wanted) {
        WINTRUST_FILE_INFO file = {};
        file.cbStruct = sizeof(file);
        file.pcwszFilePath = path.c_str();
        WINTRUST_DATA data = {};
        data.cbStruct = sizeof(data);
        data.dwUIChoice = WTD_UI_NONE;
        // Revocation is not asked about: that would go to the network each time.
        data.fdwRevocationChecks = WTD_REVOKE_NONE;
        data.dwUnionChoice = WTD_CHOICE_FILE;
        data.pFile = &file;
        data.dwStateAction = WTD_STATEACTION_VERIFY;
        data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        const LONG trust = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
        bool signed_by = false;
        if (trust == ERROR_SUCCESS) {
            // ...and by them, not merely by someone.
            CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
            CRYPT_PROVIDER_SGNR* signer =
                provider != nullptr ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
            if (signer != nullptr && signer->csCertChain > 0 && signer->pasCertChain[0].pCert) {
                char organisation[256] = {};
                CertGetNameStringA(signer->pasCertChain[0].pCert, CERT_NAME_ATTR_TYPE, 0,
                                   const_cast<char*>(szOID_ORGANIZATION_NAME), organisation, 256);
                signed_by = strcmp(organisation, wanted) == 0;
            }
        }
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
        return signed_by;
    }

    // Once per process, and never let go of: DirectML and ONNX Runtime keep threads and device
    // objects of their own, and code unloaded under them is a crash at some later moment.
    bool OnnxRuntime::Load(const std::wstring& folder, std::string* error) {
        static std::mutex lock;
        std::lock_guard<std::mutex> guard(lock);
        if (loaded()) {
            if (_wcsicmp(folder.c_str(), folder_.c_str()) != 0) {
                *error = "ONNX Runtime is already loaded from another folder";
                return false;
            }
            return true;
        }
        std::wstring paths[3];
        for (int i = 0; i < 3; ++i) {
            paths[i] = folder + L"\\" + std::wstring(kFiles[i], kFiles[i] + strlen(kFiles[i]));
            if (!Exists(paths[i])) {
                *error = std::string(kFiles[i]) + " is not beside the emulator";
                return false;
            }
        }
        // Microsoft's signature, before a line of any of them runs: a DLL beside the executable
        // could be anyone's.
        for (int i = 0; i < 3; ++i) {
            if (!SignedByMicrosoft(paths[i])) {
                *error = std::string(kFiles[i]) + " is not signed by Microsoft";
                return false;
            }
        }
        HMODULE directml = LoadLibraryExW(paths[0].c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (directml == nullptr) {
            *error = "DirectML.dll could not be loaded";
            return false;
        }
        void* create_dml_device = reinterpret_cast<void*>(GetProcAddress(directml, "DMLCreateDevice"));
        HMODULE runtime = LoadLibraryExW(paths[2].c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (runtime == nullptr) {
            *error = "onnxruntime.dll could not be loaded";
            return false;
        }
        auto get_api_base =
            reinterpret_cast<GetApiBaseFunction>(GetProcAddress(runtime, "OrtGetApiBase"));
        if (create_dml_device == nullptr || get_api_base == nullptr) {
            *error = create_dml_device == nullptr ? "DirectML.dll has no DMLCreateDevice"
                                                  : "onnxruntime.dll has no OrtGetApiBase";
            return false;
        }
        const OrtApiBase* base = get_api_base();
        const OrtApi* api = base != nullptr ? base->GetApi(ORT_API_VERSION) : nullptr;
        if (api == nullptr) {
            *error = "onnxruntime.dll does not offer the API these headers are for (version " +
                     std::to_string(ORT_API_VERSION) + ")";
            return false;
        }
        const void* dml = nullptr;
        OrtStatus* status = api->GetExecutionProviderApi("DML", ORT_API_VERSION, &dml);
        if (status != nullptr || dml == nullptr) {
            *error = "ONNX Runtime has no DirectML provider";
            if (status != nullptr)
                api->ReleaseStatus(status);
            return false;
        }
        OrtEnv* env = nullptr;
        status = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "psxemu", &env);
        if (status != nullptr) {
            *error = "ONNX Runtime would not start: " + std::string(api->GetErrorMessage(status));
            api->ReleaseStatus(status);
            return false;
        }
        version_ = base->GetVersionString();
        create_dml_device_ = create_dml_device;
        dml_ = static_cast<const OrtDmlApi*>(dml);
        env_ = env;
        folder_ = folder;
        api_ = api;   // last: loaded() from here on
        return true;
    }

    HRESULT OnnxRuntime::CreateDmlDevice(ID3D12Device* device, REFIID iid, void** dml_device) const {
        if (create_dml_device_ == nullptr)
            return E_FAIL;
        return reinterpret_cast<CreateDmlDeviceFunction>(create_dml_device_)(
            device, DML_CREATE_DEVICE_FLAG_NONE, iid, dml_device);
    }

    std::string OnnxRuntime::Error(OrtStatus* status) const {
        if (status == nullptr)
            return std::string();
        std::string text = api_->GetErrorMessage(status);
        api_->ReleaseStatus(status);
        return text;
    }

    std::unique_ptr<OnnxUpscaler> OnnxUpscaler::Create(const std::wstring& path,
                                                       ID3D12Device* device, std::string* error) {
        OnnxRuntime& runtime = OnnxRuntime::Get();
        if (!runtime.loaded()) {
            *error = "ONNX Runtime is not loaded";
            return nullptr;
        }
        const OrtApi* api = runtime.api();
        std::unique_ptr<OnnxUpscaler> upscaler(new OnnxUpscaler());

        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&upscaler->queue_)))) {
            *error = "no compute queue on the card";
            return nullptr;
        }
        Microsoft::WRL::ComPtr<IDMLDevice> dml_device;
        if (FAILED(runtime.CreateDmlDevice(device, IID_PPV_ARGS(&dml_device)))) {
            *error = "DirectML would not make a device on the card";
            return nullptr;
        }
        upscaler->dml_device_ = dml_device;

        // DirectML's provider wants memory patterns off and one node at a time (its own
        // documentation); the graph is optimised as far as ONNX Runtime goes.
        OrtSessionOptions* options = nullptr;
        std::string failed = runtime.Error(api->CreateSessionOptions(&options));
        if (failed.empty())
            failed = runtime.Error(api->DisableMemPattern(options));
        if (failed.empty())
            failed = runtime.Error(api->SetSessionExecutionMode(options, ORT_SEQUENTIAL));
        if (failed.empty())
            failed = runtime.Error(api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));
        if (failed.empty())
            failed = runtime.Error(runtime.dml()->SessionOptionsAppendExecutionProvider_DML1(
                options, dml_device.Get(), upscaler->queue_.Get()));
        if (failed.empty())
            failed = runtime.Error(
                api->CreateSession(runtime.env(), path.c_str(), options, &upscaler->session_));
        if (options != nullptr)
            api->ReleaseSessionOptions(options);
        if (!failed.empty()) {
            *error = failed;
            return nullptr;
        }

        size_t inputs = 0, outputs = 0;
        api->SessionGetInputCount(upscaler->session_, &inputs);
        api->SessionGetOutputCount(upscaler->session_, &outputs);
        if (inputs != 1 || outputs != 1) {
            *error = "the model has " + std::to_string(inputs) + " inputs and " +
                     std::to_string(outputs) + " outputs, not one of each";
            return nullptr;
        }
        OrtAllocator* allocator = nullptr;
        api->GetAllocatorWithDefaultOptions(&allocator);
        char* name = nullptr;
        api->SessionGetInputName(upscaler->session_, 0, allocator, &name);
        upscaler->input_name_ = name;
        allocator->Free(allocator, name);
        api->SessionGetOutputName(upscaler->session_, 0, allocator, &name);
        upscaler->output_name_ = name;
        allocator->Free(allocator, name);

        // The input's shape and type: NCHW, three channels, float or half.
        OrtTypeInfo* type = nullptr;
        api->SessionGetInputTypeInfo(upscaler->session_, 0, &type);
        const OrtTensorTypeAndShapeInfo* tensor = nullptr;
        api->CastTypeInfoToTensorInfo(type, &tensor);
        ONNXTensorElementDataType element = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
        size_t rank = 0;
        int64_t dims[8] = {};
        if (tensor != nullptr) {
            api->GetTensorElementType(tensor, &element);
            api->GetDimensionsCount(tensor, &rank);
            if (rank == 4)
                api->GetDimensions(tensor, dims, 4);
        }
        api->ReleaseTypeInfo(type);
        if (rank != 4 || (dims[1] != 3 && dims[1] > 0)) {
            *error = "the model's input is not a picture of three channels";
            return nullptr;
        }
        if (element != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
            element != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            *error = "the model's input is neither float nor half";
            return nullptr;
        }
        upscaler->half_ = element == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
        upscaler->fixed_height_ = dims[2] > 0 ? static_cast<int>(dims[2]) : 0;
        upscaler->fixed_width_ = dims[3] > 0 ? static_cast<int>(dims[3]) : 0;

        failed = runtime.Error(
            api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &upscaler->memory_));
        if (!failed.empty()) {
            *error = failed;
            return nullptr;
        }
        return upscaler;
    }

    std::string OnnxUpscaler::description() const {
        std::string text = "ONNX Runtime " + OnnxRuntime::Get().version() + " on DirectML, input ";
        text += fixed_width_ != 0 ? std::to_string(fixed_width_) + "x" +
                                        std::to_string(fixed_height_) + " only"
                                  : "any size";
        return text + (half_ ? ", half floats" : ", floats");
    }

    OnnxUpscaler::~OnnxUpscaler() {
        const OrtApi* api = OnnxRuntime::Get().api();
        if (api == nullptr)
            return;
        if (memory_ != nullptr)
            api->ReleaseMemoryInfo(memory_);
        if (session_ != nullptr)
            api->ReleaseSession(session_);
    }

    bool OnnxUpscaler::Run(const std::vector<float>& rgb, int width, int height,
                           std::vector<float>* out, int* out_width, int* out_height,
                           std::string* error) {
        OnnxRuntime& runtime = OnnxRuntime::Get();
        const OrtApi* api = runtime.api();
        const size_t count = static_cast<size_t>(width) * height * 3;
        if (rgb.size() != count) {
            *error = "the picture is not width x height x 3";
            return false;
        }
        if ((fixed_width_ != 0 && fixed_width_ != width) ||
            (fixed_height_ != 0 && fixed_height_ != height)) {
            *error = "the model takes only " + std::to_string(fixed_width_) + "x" +
                     std::to_string(fixed_height_);
            return false;
        }
        const int64_t shape[4] = { 1, 3, height, width };
        std::vector<uint16_t> halves;
        void* data = const_cast<float*>(rgb.data());
        size_t bytes = count * sizeof(float);
        if (half_) {
            halves.resize(count);
            for (size_t i = 0; i < count; ++i)
                halves[i] = DirectX::PackedVector::XMConvertFloatToHalf(rgb[i]);
            data = halves.data();
            bytes = count * sizeof(uint16_t);
        }
        OrtValue* input = nullptr;
        std::string failed = runtime.Error(api->CreateTensorWithDataAsOrtValue(
            memory_, data, bytes, shape, 4,
            half_ ? ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 : ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
            &input));
        OrtValue* output = nullptr;
        if (failed.empty()) {
            const char* in_name = input_name_.c_str();
            const char* out_name = output_name_.c_str();
            failed = runtime.Error(api->Run(session_, nullptr, &in_name, &input, 1, &out_name, 1,
                                            &output));
        }
        if (input != nullptr)
            api->ReleaseValue(input);
        if (!failed.empty()) {
            *error = failed;
            return false;
        }

        OrtTensorTypeAndShapeInfo* info = nullptr;
        api->GetTensorTypeAndShape(output, &info);
        size_t rank = 0;
        int64_t dims[8] = {};
        ONNXTensorElementDataType element = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
        api->GetTensorElementType(info, &element);
        api->GetDimensionsCount(info, &rank);
        if (rank == 4)
            api->GetDimensions(info, dims, 4);
        api->ReleaseTensorTypeAndShapeInfo(info);
        const bool half_out = element == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;
        const int scale = rank == 4 && dims[3] % width == 0 ? static_cast<int>(dims[3] / width) : 0;
        if (rank != 4 || dims[1] != 3 || scale == 0 || dims[2] != static_cast<int64_t>(height) * scale) {
            api->ReleaseValue(output);
            *error = "the model's output is not the picture a whole number of times larger";
            return false;
        }
        scale_ = scale;
        *out_width = width * scale;
        *out_height = height * scale;
        const size_t out_count = static_cast<size_t>(*out_width) * *out_height * 3;
        out->resize(out_count);
        void* result = nullptr;
        api->GetTensorMutableData(output, &result);
        if (half_out) {
            const uint16_t* h = static_cast<const uint16_t*>(result);
            for (size_t i = 0; i < out_count; ++i)
                (*out)[i] = DirectX::PackedVector::XMConvertHalfToFloat(h[i]);
        } else {
            memcpy(out->data(), result, out_count * sizeof(float));
        }
        api->ReleaseValue(output);
        return true;
    }

}   // namespace emulation::win32
