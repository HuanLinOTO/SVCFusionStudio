#include "OnnxRuntimeLoader.h"

#include "../JuceHeader.h"
#include "AppLogger.h"
#include <onnxruntime_cxx_api.h>

#include <mutex>

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace OnnxRuntimeLoader {

#if JUCE_WINDOWS
namespace {

juce::File getOwningModuleDirectory() {
  HMODULE moduleHandle = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&ensureLoaded), &moduleHandle) &&
      moduleHandle != nullptr) {
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(moduleHandle, modulePath, MAX_PATH) > 0)
      return juce::File(juce::String(modulePath)).getParentDirectory();
  }

  return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
      .getParentDirectory();
}

bool loadOptionalDll(const juce::File &dllPath) {
  if (!dllPath.existsAsFile())
    return true;

  auto handle = LoadLibraryExW(dllPath.getFullPathName().toWideCharPointer(),
                               nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!handle) {
    LOG("ONNX Runtime loader: failed to preload " +
        dllPath.getFullPathName() + " (error=" +
        juce::String(static_cast<int>(GetLastError())) + ")");
    return false;
  }

  return true;
}

} // namespace
#endif

bool ensureLoaded() {
#if JUCE_WINDOWS
  static std::once_flag once;
  static bool loaded = false;

  std::call_once(once, []() {
    const auto moduleDir = getOwningModuleDirectory();
    const auto ortDll = moduleDir.getChildFile("onnxruntime.dll");

    if (!ortDll.existsAsFile()) {
      LOG("ONNX Runtime loader: bundled onnxruntime.dll not found at " +
          ortDll.getFullPathName());
      loaded = false;
      return;
    }

    // DirectML is an optional dependency that some packaged builds ship next
    // to the ORT runtime.
    loadOptionalDll(moduleDir.getChildFile("DirectML.dll"));

    if (!loadOptionalDll(ortDll)) {
      loaded = false;
      return;
    }

    const OrtApiBase *apiBase = OrtGetApiBase();
    if (apiBase == nullptr) {
      LOG("ONNX Runtime loader: OrtGetApiBase returned null");
      loaded = false;
      return;
    }

    const OrtApi *api = apiBase->GetApi(ORT_API_VERSION);
    if (api == nullptr) {
      LOG("ONNX Runtime loader: GetApi(" + juce::String(ORT_API_VERSION) +
          ") returned null");
      loaded = false;
      return;
    }

    Ort::InitApi(api);

    // Providers shared is loaded lazily by ONNX Runtime, but preloading it from
    // the same directory keeps Windows away from unrelated global copies.
    loadOptionalDll(moduleDir.getChildFile("onnxruntime_providers_shared.dll"));

    loaded = true;
    LOG("ONNX Runtime loader: preloaded " + ortDll.getFullPathName() +
        " (version=" + juce::String(apiBase->GetVersionString()) + ")");
  });

  return loaded;
#else
  return true;
#endif
}

} // namespace OnnxRuntimeLoader