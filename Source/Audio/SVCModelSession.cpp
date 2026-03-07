#include "SVCModelSession.h"
#include "../Utils/AppLogger.h"

#include <thread>

#ifdef HAVE_ONNXRUNTIME
#ifdef USE_DIRECTML
#include <dml_provider_factory.h>
#endif
#endif

SVCModelSession::SVCModelSession() = default;

SVCModelSession::~SVCModelSession() { unload(); }

void SVCModelSession::unload()
{
#ifdef HAVE_ONNXRUNTIME
    encoderSession.reset();
    velocitySession.reset();
    sovitsSession.reset();
#endif
    loaded = false;

    // Clean up temp directory only if we extracted to temp (not persistent dir)
    if (!loadedFromDir && tempDir.isDirectory())
        tempDir.deleteRecursively();
    loadedFromDir = false;
}

bool SVCModelSession::parseConfig(const juce::File& sfsFile)
{
    juce::ZipFile zip(sfsFile);
    if (zip.getNumEntries() == 0)
        return false;

    auto idx = zip.getIndexOfFileName("config.json");
    if (idx < 0)
        return false;

    std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(idx));
    if (!stream)
        return false;

    auto jsonStr = stream->readEntireStreamAsString();
    auto parsed = juce::JSON::parse(jsonStr);
    auto* obj = parsed.getDynamicObject();
    if (!obj)
        return false;

    config.formatVersion   = static_cast<int>(obj->getProperty("format_version"));
    config.modelTypeIndex  = static_cast<int>(obj->getProperty("model_type_index"));
    config.modelTypeName   = obj->getProperty("model_type_name").toString();
    config.name            = obj->getProperty("name").toString();
    config.description     = obj->getProperty("description").toString();
    config.encoder         = obj->getProperty("encoder").toString();
    config.sampleRate      = static_cast<int>(obj->getProperty("sample_rate"));
    config.blockSize       = static_cast<int>(obj->getProperty("block_size"));
    config.nSpk            = static_cast<int>(obj->getProperty("n_spk"));
    config.nHidden         = static_cast<int>(obj->getProperty("n_hidden"));
    config.velocityTType   = obj->getProperty("velocity_t_type").toString();

    if (obj->hasProperty("speaker_names"))
    {
        if (auto* arr = obj->getProperty("speaker_names").getArray())
            for (const auto& item : *arr)
                config.speakerNames.add(item.toString());
    }

    // Load avatar image from avatar.png in the zip
    {
        auto avatarIdx = zip.getIndexOfFileName("avatar.png");
        if (avatarIdx >= 0)
        {
            std::unique_ptr<juce::InputStream> avatarStream(zip.createStreamForEntry(avatarIdx));
            if (avatarStream)
                config.avatar = juce::ImageFileFormat::loadFrom(*avatarStream);
        }
    }

    return config.modelTypeIndex >= 0;
}

bool SVCModelSession::parseConfigFromDir(const juce::File& directory)
{
    auto configFile = directory.getChildFile("config.json");
    if (!configFile.existsAsFile())
        return false;

    auto jsonStr = configFile.loadFileAsString();
    auto parsed = juce::JSON::parse(jsonStr);
    auto* obj = parsed.getDynamicObject();
    if (!obj)
        return false;

    config.formatVersion   = static_cast<int>(obj->getProperty("format_version"));
    config.modelTypeIndex  = static_cast<int>(obj->getProperty("model_type_index"));
    config.modelTypeName   = obj->getProperty("model_type_name").toString();
    config.name            = obj->getProperty("name").toString();
    config.encoder         = obj->getProperty("encoder").toString();
    config.sampleRate      = static_cast<int>(obj->getProperty("sample_rate"));
    config.blockSize       = static_cast<int>(obj->getProperty("block_size"));
    config.nSpk            = static_cast<int>(obj->getProperty("n_spk"));
    config.nHidden         = static_cast<int>(obj->getProperty("n_hidden"));
    config.velocityTType   = obj->getProperty("velocity_t_type").toString();

    if (obj->hasProperty("speaker_names"))
    {
        if (auto* arr = obj->getProperty("speaker_names").getArray())
            for (const auto& item : *arr)
                config.speakerNames.add(item.toString());
    }

    // Load avatar image from avatar.png in the directory
    auto avatarFile = directory.getChildFile("avatar.png");
    if (avatarFile.existsAsFile())
        config.avatar = juce::ImageFileFormat::loadFrom(avatarFile);

    return config.modelTypeIndex >= 0;
}

bool SVCModelSession::loadFromSfsModel(const juce::File& sfsModelFile,
                                       const juce::String& executionDevice,
                                       int deviceId)
{
    unload();

    if (!sfsModelFile.existsAsFile())
    {
        DBG("SVCModelSession: File not found: " + sfsModelFile.getFullPathName());
        return false;
    }

    // Parse config first
    if (!parseConfig(sfsModelFile))
    {
        DBG("SVCModelSession: Failed to parse config from " + sfsModelFile.getFullPathName());
        return false;
    }

    // Extract and load ONNX files
    if (!extractAndLoadOnnx(sfsModelFile, executionDevice, deviceId))
    {
        DBG("SVCModelSession: Failed to load ONNX sessions");
        return false;
    }

    loaded = true;
    DBG("SVCModelSession: Loaded " + config.modelTypeName + " (" + config.name + ")");
    return true;
}

bool SVCModelSession::loadFromDirectory(const juce::File& directory,
                                        const juce::String& executionDevice,
                                        int deviceId)
{
    unload();

    if (!directory.isDirectory())
    {
        DBG("SVCModelSession: Directory not found: " + directory.getFullPathName());
        return false;
    }

    if (!parseConfigFromDir(directory))
    {
        DBG("SVCModelSession: Failed to parse config from " + directory.getFullPathName());
        return false;
    }

    if (!loadOnnxFromDir(directory, executionDevice, deviceId))
    {
        DBG("SVCModelSession: Failed to load ONNX sessions from directory");
        return false;
    }

    loadedFromDir = true;
    loaded = true;
    DBG("SVCModelSession: Loaded from dir " + config.modelTypeName + " (" + config.name + ")");
    return true;
}

bool SVCModelSession::loadOnnxFromDir(const juce::File& directory,
                                      const juce::String& executionDevice,
                                      int deviceId)
{
#ifdef HAVE_ONNXRUNTIME
    bool isSoVITS = (config.modelTypeIndex == 2);

    // Verify required ONNX files exist
    if (isSoVITS)
    {
        if (!directory.getChildFile("sovits.onnx").existsAsFile())
        {
            DBG("SVCModelSession: sovits.onnx not found in " + directory.getFullPathName());
            return false;
        }
    }
    else
    {
        if (!directory.getChildFile("encoder.onnx").existsAsFile())
        {
            DBG("SVCModelSession: encoder.onnx not found in " + directory.getFullPathName());
            return false;
        }
        if (!directory.getChildFile("velocity.onnx").existsAsFile())
        {
            DBG("SVCModelSession: velocity.onnx not found in " + directory.getFullPathName());
            return false;
        }
    }

    // Initialize ONNX Runtime
    if (!onnxEnv)
    {
        try {
            onnxEnv = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "SVCModel");
        } catch (const Ort::Exception& e) {
            DBG("SVCModelSession: Failed to init ONNX env: " + juce::String(e.what()));
            return false;
        }
    }

    auto sessionOpts = createSessionOptions(executionDevice, deviceId);

    try
    {
        if (isSoVITS)
        {
            auto sovitsPath = directory.getChildFile("sovits.onnx");
#ifdef _WIN32
            std::wstring wpath(sovitsPath.getFullPathName().toWideCharPointer());
            sovitsSession = std::make_unique<Ort::Session>(*onnxEnv, wpath.c_str(), sessionOpts);
#else
            sovitsSession = std::make_unique<Ort::Session>(*onnxEnv, sovitsPath.getFullPathName().toRawUTF8(), sessionOpts);
#endif
            DBG("SVCModelSession: sovits.onnx session created from directory");
        }
        else
        {
            auto encoderPath = directory.getChildFile("encoder.onnx");
            auto velocityPath = directory.getChildFile("velocity.onnx");

            // Encoder uses CPU (DirectML causes severe perf regression on encoder graphs).
            // Velocity uses GPU (DirectML/CUDA) since it runs fast there.
            auto encoderOpts = createSessionOptions("CPU", 0);
            auto velocityOpts = createSessionOptions(executionDevice, deviceId);

#ifdef _WIN32
            std::wstring wEnc(encoderPath.getFullPathName().toWideCharPointer());
            std::wstring wVel(velocityPath.getFullPathName().toWideCharPointer());
            encoderSession = std::make_unique<Ort::Session>(*onnxEnv, wEnc.c_str(), encoderOpts);
            velocitySession = std::make_unique<Ort::Session>(*onnxEnv, wVel.c_str(), velocityOpts);
#else
            encoderSession = std::make_unique<Ort::Session>(*onnxEnv, encoderPath.getFullPathName().toRawUTF8(), encoderOpts);
            velocitySession = std::make_unique<Ort::Session>(*onnxEnv, velocityPath.getFullPathName().toRawUTF8(), velocityOpts);
#endif
            LOG("SVCModelSession: encoder (CPU) + velocity (" + executionDevice + ") sessions created from directory");
        }

        return true;
    }
    catch (const Ort::Exception& e)
    {
        DBG("SVCModelSession: ONNX session creation failed: " + juce::String(e.what()));
        return false;
    }
#else
    juce::ignoreUnused(directory, executionDevice, deviceId);
    DBG("SVCModelSession: ONNX Runtime not available");
    return false;
#endif
}

bool SVCModelSession::extractAndLoadOnnx(const juce::File& sfsFile,
                                         const juce::String& executionDevice,
                                         int deviceId)
{
#ifdef HAVE_ONNXRUNTIME
    // Create a unique temp directory
    tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getChildFile("SVCFusionStudio_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    tempDir.createDirectory();

    juce::ZipFile zip(sfsFile);

    // Determine which ONNX files to extract
    bool isSoVITS = (config.modelTypeIndex == 2);
    juce::StringArray onnxFiles;

    if (isSoVITS)
        onnxFiles.add("sovits.onnx");
    else
    {
        onnxFiles.add("encoder.onnx");
        onnxFiles.add("velocity.onnx");
    }

    // Extract ONNX files
    for (const auto& name : onnxFiles)
    {
        auto entryIdx = zip.getIndexOfFileName(name);
        if (entryIdx < 0)
        {
            DBG("SVCModelSession: Missing ONNX file in archive: " + name);
            return false;
        }

        auto targetFile = tempDir.getChildFile(name);
        auto result = zip.uncompressEntry(entryIdx, tempDir);
        if (result.failed() || !targetFile.existsAsFile())
        {
            DBG("SVCModelSession: Failed to extract " + name);
            return false;
        }
        DBG("SVCModelSession: Extracted " + name + " (" +
            juce::String(targetFile.getSize() / 1024 / 1024) + " MB)");
    }

    // Initialize ONNX Runtime
    if (!onnxEnv)
    {
        try {
            onnxEnv = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "SVCModel");
        } catch (const Ort::Exception& e) {
            DBG("SVCModelSession: Failed to init ONNX env: " + juce::String(e.what()));
            return false;
        }
    }

    auto sessionOpts = createSessionOptions(executionDevice, deviceId);

    try
    {
        if (isSoVITS)
        {
            auto sovitsPath = tempDir.getChildFile("sovits.onnx");
#ifdef _WIN32
            std::wstring wpath(sovitsPath.getFullPathName().toWideCharPointer());
            sovitsSession = std::make_unique<Ort::Session>(*onnxEnv, wpath.c_str(), sessionOpts);
#else
            sovitsSession = std::make_unique<Ort::Session>(*onnxEnv, sovitsPath.getFullPathName().toRawUTF8(), sessionOpts);
#endif
            DBG("SVCModelSession: sovits.onnx session created");
        }
        else
        {
            auto encoderPath = tempDir.getChildFile("encoder.onnx");
            auto velocityPath = tempDir.getChildFile("velocity.onnx");

            // Encoder uses CPU (DirectML causes severe perf regression on encoder graphs).
            // Velocity uses GPU (DirectML/CUDA) since it runs fast there.
            auto encoderOpts = createSessionOptions("CPU", 0);
            auto velocityOpts = createSessionOptions(executionDevice, deviceId);

#ifdef _WIN32
            std::wstring wEnc(encoderPath.getFullPathName().toWideCharPointer());
            std::wstring wVel(velocityPath.getFullPathName().toWideCharPointer());
            encoderSession = std::make_unique<Ort::Session>(*onnxEnv, wEnc.c_str(), encoderOpts);
            velocitySession = std::make_unique<Ort::Session>(*onnxEnv, wVel.c_str(), velocityOpts);
#else
            encoderSession = std::make_unique<Ort::Session>(*onnxEnv, encoderPath.getFullPathName().toRawUTF8(), encoderOpts);
            velocitySession = std::make_unique<Ort::Session>(*onnxEnv, velocityPath.getFullPathName().toRawUTF8(), velocityOpts);
#endif
            LOG("SVCModelSession: encoder (CPU) + velocity (" + executionDevice + ") sessions created");
        }

        return true;
    }
    catch (const Ort::Exception& e)
    {
        DBG("SVCModelSession: ONNX session creation failed: " + juce::String(e.what()));
        return false;
    }
#else
    juce::ignoreUnused(sfsFile, executionDevice, deviceId);
    DBG("SVCModelSession: ONNX Runtime not available");
    return false;
#endif
}

#ifdef HAVE_ONNXRUNTIME
Ort::SessionOptions SVCModelSession::createSessionOptions(const juce::String& device, int deviceId)
{
    Ort::SessionOptions opts;
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    if (device == "CUDA")
    {
        opts.SetIntraOpNumThreads(4);
        OrtCUDAProviderOptions cudaOpts;
        cudaOpts.device_id = deviceId;
        opts.AppendExecutionProvider_CUDA(cudaOpts);
    }
#ifdef USE_DIRECTML
    else if (device == "DirectML")
    {
        opts.SetIntraOpNumThreads(4);
        const OrtApi& ortApi = Ort::GetApi();
        const OrtDmlApi* ortDmlApi = nullptr;
        Ort::ThrowOnError(ortApi.GetExecutionProviderApi(
            "DML", ORT_API_VERSION,
            reinterpret_cast<const void**>(&ortDmlApi)));
        opts.DisableMemPattern();
        opts.SetExecutionMode(ORT_SEQUENTIAL);
        Ort::ThrowOnError(
            ortDmlApi->SessionOptionsAppendExecutionProvider_DML(
                opts, deviceId));
    }
#endif
    else
    {
        // CPU: use more threads for faster multi-threaded execution
        int numThreads = static_cast<int>(std::thread::hardware_concurrency());
        if (numThreads < 4) numThreads = 4;
        opts.SetIntraOpNumThreads(numThreads);
        opts.SetInterOpNumThreads(numThreads);
    }

    return opts;
}
#endif
