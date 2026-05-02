#pragma once

#include "../JuceHeader.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/Localization.h"
#include "../Utils/PlatformPaths.h"
#include <cmath>
#include <vector>

/**
 * VoicebankPanel - Panel for managing SVC voice models (voicebanks)
 *
 * Supports loading .sfs_model files (ZIP archives containing config.json + ONNX)
 * via file chooser dialog or drag-and-drop.
 *
 * Supported model types:
 *  0 = DDSP-SVC 6.0  (encoder.onnx + velocity.onnx)
 *  1 = Reflow-VAE-SVC (encoder.onnx + velocity.onnx)
 *  2 = So-VITS-SVC    (sovits.onnx)
 *  3 = DDSP-SVC 6.1  (encoder.onnx + velocity.onnx)
 *  4 = DDSP-SVC 6.3  (encoder.onnx + velocity.onnx)
 */
class VoicebankPanel : public juce::Component,
                       public juce::ListBoxModel,
                       public juce::FileDragAndDropTarget,
                       private juce::Timer
{
public:
    struct VoicebankInfo
    {
        juce::String name;
        juce::String description;
        juce::Image avatar;
        juce::String path;           // .sfs_model file path or directory
        int modelTypeIndex = -1;     // 0-4
        juce::String modelTypeName;  // "DDSP-SVC 6.3" etc.
        int sampleRate = 44100;
        int blockSize = 512;
        int numSpeakers = 1;
        int nHidden = 256;
        juce::StringArray speakerNames;
        juce::String encoder;        // e.g. "contentvec768l12"
        juce::String velocityTType;  // "int64", "float32", or "none"
        bool loaded = false;
    };

    VoicebankPanel();
    ~VoicebankPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    int getPreferredHeight() const { return 450; }

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g,
                          int width, int height, bool isSelected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& e) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent& e) override;

    // FileDragAndDropTarget
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;

    // API
    void loadSfsModel(const juce::File& sfsModelFile);
    void addVoicebank(const juce::File& directory);
    void removeSelectedVoicebank();
    const VoicebankInfo* getSelectedVoicebank() const;
    const VoicebankInfo* getActiveVoicebank() const;

    /** Scan the persistent voicebanks directory and populate the list. */
    void scanVoicebanksDirectory();

    // Callbacks
    std::function<void(const VoicebankInfo&)> onVoicebankActivated;
    std::function<void()> onVoicebankRemoved;

private:
    void timerCallback() override;
    void startInfoAnimation(bool emphasizeActivation);
    void applyInfoAnimationState();
    int getDisplayInfoIndex() const;
    void scanDirectory(const juce::File& dir, VoicebankInfo& info);
    bool parseSfsModelConfig(const juce::File& sfsFile, VoicebankInfo& info);
    void updateInfoDisplay();
    juce::String getModelTypeName(int typeIndex) const;

    std::vector<VoicebankInfo> voicebanks;
    int activeIndex = -1;

    // UI Components
    juce::Label titleLabel;
    juce::ListBox listBox;
    juce::TextButton addButton;
    juce::TextButton removeButton;
    juce::TextButton activateButton;

    // Info display area
    juce::Label infoNameLabel;
    juce::Label infoDescriptionLabel;
    juce::Label infoTypeLabel;
    juce::Label infoSpeakersLabel;
    juce::Label infoEncoderLabel;
    juce::Label infoSampleRateLabel;
    juce::Label infoHiddenLabel;
    juce::Image avatarImage;   // cached avatar for selected voicebank

    juce::Rectangle<int> listCardBounds;
    juce::Rectangle<int> infoCardBounds;
    juce::Rectangle<int> avatarBounds;

    bool isDragOver = false;
    bool infoAnimationActive = false;
    bool activationBurst = false;
    float infoAnimationProgress = 1.0f;
    double infoAnimationStartTimeMs = 0.0;
    int infoAnimationDurationMs = 260;
    int displayedInfoIndex = -1;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoicebankPanel)
};
