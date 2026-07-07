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
  static std::once_flag once;
  static bool loaded = false;

  std::call_once(once, []() {
#if JUCE_WINDOWS
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
#endif

    // Verify ONNX Runtime C API is reachable on every platform. Earlier
    // versions (e.g. 1.17.x) have been observed to return a null apiBase on
    // newer macOS builds (macOS 27 Tahoe), which previously led to a NULL
    // pointer dereference crash inside Ort::Env construction. Catching it
    // here lets callers fall back gracefully instead of segfaulting.
    const OrtApiBase *apiBase = OrtGetApiBase();
    if (apiBase == nullptr) {
      LOG("ONNX Runtime loader: OrtGetApiBase returned null");
      loaded = false;
      return;
    }

    const OrtApi *api = apiBase->GetApi(ORT_API_VERSION);
    if (api == nullptr) {
      LOG("ONNX Runtime loader: GetApi(" + juce::String(ORT_API_VERSION) +
          ") returned null (runtime version=" +
          juce::String(apiBase->GetVersionString()) + ")");
      loaded = false;
      return;
    }

    Ort::InitApi(api);

#if JUCE_WINDOWS
    // Providers shared is loaded lazily by ONNX Runtime, but preloading it from
    // the same directory keeps Windows away from unrelated global copies.
    loadOptionalDll(getOwningModuleDirectory().getChildFile(
        "onnxruntime_providers_shared.dll"));
#endif

    loaded = true;
    LOG("ONNX Runtime loader: ready (version=" +
        juce::String(apiBase->GetVersionString()) + ")");
  });

  return loaded;
}

} // namespace OnnxRuntimeLoader