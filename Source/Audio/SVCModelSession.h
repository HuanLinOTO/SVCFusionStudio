#pragma once

#include "../JuceHeader.h"
#include <memory>
#include <string>
#include <vector>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

/**
 * Manages ONNX sessions loaded from a .sfs_model (ZIP) file.
 *
 * Model types:
 *  0 = DDSP-SVC 6.0   (encoder.onnx + velocity.onnx)
 *  1 = Reflow-VAE-SVC  (encoder.onnx + velocity.onnx)
 *  2 = So-VITS-SVC     (sovits.onnx)
 *  3 = DDSP-SVC 6.1   (encoder.onnx + velocity.onnx)
 *  4 = DDSP-SVC 6.3   (encoder.onnx + velocity.onnx)
 */
class SVCModelSession
{
public:
    struct ModelConfig
    {
        int formatVersion = 1;
        int modelTypeIndex = -1;
        juce::String modelTypeName;
        juce::String name;
        juce::String description;      // optional description text
        juce::String encoder;          // e.g. "contentvec768l12"
        int sampleRate = 44100;
        int blockSize = 512;
        int nSpk = 1;
        int nHidden = 256;
        juce::StringArray speakerNames;
        juce::String velocityTType;    // "int64", "float32", or "none"
        juce::Image avatar;            // optional avatar image from avatar.png
    };

    SVCModelSession();
    ~SVCModelSession();

    /**
     * Load model from .sfs_model file (ZIP containing config.json + ONNX).
     * Extracts ONNX files to a temp directory and creates ONNX sessions.
     */
    bool loadFromSfsModel(const juce::File& sfsModelFile,
                          const juce::String& executionDevice = "CPU",
                          int deviceId = 0);

    /**
     * Load model from an already-extracted directory containing config.json + ONNX.
     * Used when loading from the persistent voicebanks directory.
     */
    bool loadFromDirectory(const juce::File& directory,
                           const juce::String& executionDevice = "CPU",
                           int deviceId = 0);

    /** Unload all sessions and clean up temp files. */
    void unload();

    bool isLoaded() const { return loaded; }
    const ModelConfig& getConfig() const { return config; }

    /** Check if this model type uses encoder+velocity (vs. single sovits.onnx). */
    bool hasEncoderVelocity() const { return config.modelTypeIndex != 2; }

    /** Check if velocity input 't' is int64 (true) or float32 (false). */
    bool isVelocityTInt64() const { return config.velocityTType == "int64"; }

#ifdef HAVE_ONNXRUNTIME
    Ort::Session* getEncoderSession() const { return encoderSession.get(); }
    Ort::Session* getVelocitySession() const { return velocitySession.get(); }
    Ort::Session* getSovitsSession() const { return sovitsSession.get(); }
#endif

private:
    bool parseConfig(const juce::File& sfsFile);
    bool parseConfigFromDir(const juce::File& directory);
    bool extractAndLoadOnnx(const juce::File& sfsFile,
                            const juce::String& executionDevice,
                            int deviceId);
    bool loadOnnxFromDir(const juce::File& directory,
                         const juce::String& executionDevice,
                         int deviceId);

    ModelConfig config;
    bool loaded = false;
    bool loadedFromDir = false; // true if loaded from persistent dir (no temp cleanup)
    juce::File tempDir;

#ifdef HAVE_ONNXRUNTIME
    std::shared_ptr<Ort::Env> onnxEnv;
    std::unique_ptr<Ort::Session> encoderSession;
    std::unique_ptr<Ort::Session> velocitySession;
    std::unique_ptr<Ort::Session> sovitsSession;

    Ort::SessionOptions createSessionOptions(const juce::String& device, int deviceId);
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SVCModelSession)
};
