#include "VoicebankPanel.h"
#include "StyledComponents.h"
#include "../Utils/Localization.h"
#include "../Utils/PlatformPaths.h"
#include <set>

VoicebankPanel::VoicebankPanel()
{
    // Title
    addAndMakeVisible(titleLabel);
    titleLabel.setText(TR("voicebank.title"), juce::dontSendNotification);
    titleLabel.setColour(juce::Label::textColourId, APP_COLOR_PRIMARY);
    titleLabel.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));

    // List box
    addAndMakeVisible(listBox);
    listBox.setModel(this);
    listBox.setRowHeight(48);
    listBox.setColour(juce::ListBox::backgroundColourId, APP_COLOR_SURFACE);
    listBox.setColour(juce::ListBox::outlineColourId, APP_COLOR_BORDER);
    listBox.setOutlineThickness(1);

    // Buttons
    addAndMakeVisible(addButton);
    addAndMakeVisible(removeButton);
    addAndMakeVisible(activateButton);

    addButton.setButtonText(TR("voicebank.add"));
    removeButton.setButtonText(TR("voicebank.remove"));
    activateButton.setButtonText(TR("voicebank.activate"));

    // Primary style for add button
    addButton.setColour(juce::TextButton::buttonColourId, APP_COLOR_PRIMARY);
    addButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    addButton.setColour(juce::TextButton::buttonOnColourId, APP_COLOR_PRIMARY.brighter(0.2f));

    // Secondary style for other buttons
    removeButton.setColour(juce::TextButton::buttonColourId, APP_COLOR_SURFACE_ALT);
    removeButton.setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_PRIMARY);
    activateButton.setColour(juce::TextButton::buttonColourId, APP_COLOR_SURFACE_ALT);
    activateButton.setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_PRIMARY);

    // Info labels
    for (auto* label : { &infoNameLabel, &infoDescriptionLabel, &infoTypeLabel,
                         &infoSpeakersLabel, &infoEncoderLabel, &infoSampleRateLabel,
                         &infoHiddenLabel })
    {
        addAndMakeVisible(label);
        label->setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    }
    infoNameLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    infoNameLabel.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    infoDescriptionLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    infoDescriptionLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED.brighter(0.2f));

    // Button callbacks
    addButton.onClick = [this]()
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            TR("voicebank.select_file"),
            juce::File::getSpecialLocation(juce::File::userHomeDirectory),
            "*.sfs_model");

        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
            {
                auto result = fc.getResult();
                if (result.existsAsFile())
                    loadSfsModel(result);
            });
    };

    removeButton.onClick = [this]() { removeSelectedVoicebank(); };

    activateButton.onClick = [this]()
    {
        int selected = listBox.getSelectedRow();
        if (selected >= 0 && selected < static_cast<int>(voicebanks.size()))
        {
            activeIndex = selected;
            listBox.repaint();
            updateInfoDisplay();
            startInfoAnimation(true);

            if (onVoicebankActivated)
                onVoicebankActivated(voicebanks[static_cast<size_t>(activeIndex)]);
        }
    };

    updateInfoDisplay();
}

VoicebankPanel::~VoicebankPanel() = default;

// ==== Paint ====

void VoicebankPanel::paint(juce::Graphics& g)
{
    auto drawCard = [&g](const juce::Rectangle<int>& bounds)
    {
        if (bounds.isEmpty())
            return;
        const float radius = 10.0f;
        g.setColour(APP_COLOR_SURFACE_RAISED);
        g.fillRoundedRectangle(bounds.toFloat(), radius);

        juce::Path borderPath;
        borderPath.addRoundedRectangle(bounds.toFloat().reduced(0.5f), radius);
        juce::ColourGradient borderGradient(
            APP_COLOR_BORDER_HIGHLIGHT, static_cast<float>(bounds.getX()), static_cast<float>(bounds.getY()),
            APP_COLOR_BORDER.darker(0.3f), static_cast<float>(bounds.getRight()), static_cast<float>(bounds.getBottom()), false);
        g.setGradientFill(borderGradient);
        g.strokePath(borderPath, juce::PathStrokeType(1.1f));

        g.setColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.4f));
        g.drawRoundedRectangle(bounds.toFloat().reduced(1.2f), radius - 1.0f, 0.6f);
    };

    drawCard(listCardBounds);
    drawCard(infoCardBounds);

    if (infoAnimationActive && !infoCardBounds.isEmpty())
    {
        auto card = infoCardBounds.toFloat().reduced(1.5f);
        const auto glowAlpha = activationBurst ? 0.22f : 0.14f;
        const auto shimmerWidth = juce::jmax(46.0f, card.getWidth() * 0.22f);
        const auto shimmerCentreX = juce::jmap(infoAnimationProgress, card.getX() - shimmerWidth,
                                               card.getRight() + shimmerWidth);
        juce::ColourGradient shimmer(
            APP_COLOR_PRIMARY.withAlpha(0.0f), shimmerCentreX - shimmerWidth, card.getY(),
            APP_COLOR_PRIMARY.withAlpha(0.0f), shimmerCentreX + shimmerWidth, card.getBottom(), false);
        shimmer.addColour(0.5, juce::Colours::white.withAlpha(glowAlpha));
        g.setGradientFill(shimmer);
        g.fillRoundedRectangle(card, 10.0f);
    }

    // Draw avatar in the info card
    if (!avatarBounds.isEmpty())
    {
        auto avatarArea = avatarBounds.toFloat();
        const auto entryScale = juce::jmap(infoAnimationProgress, 0.0f, 1.0f,
                                           activationBurst ? 0.78f : 0.88f, 1.0f);
        const auto overshoot = activationBurst
                                   ? 1.0f + 0.07f * std::sin(infoAnimationProgress * juce::MathConstants<float>::pi)
                                   : 1.0f;
        const auto avatarScale = entryScale * overshoot;
        const auto scaledWidth = avatarArea.getWidth() * avatarScale;
        const auto scaledHeight = avatarArea.getHeight() * avatarScale;
        avatarArea = juce::Rectangle<float>(scaledWidth, scaledHeight)
                         .withCentre(avatarArea.getCentre())
                         .translated(0.0f, juce::jmap(infoAnimationProgress, 10.0f, 0.0f));

        if (infoAnimationActive)
        {
            const auto haloAlpha = activationBurst ? 0.32f : 0.18f;
            g.setColour(APP_COLOR_PRIMARY.withAlpha(haloAlpha * (1.0f - infoAnimationProgress * 0.35f)));
            g.fillEllipse(avatarArea.expanded(10.0f, 10.0f));
        }

        if (avatarImage.isValid())
        {
            g.drawImageWithin(avatarImage, juce::roundToInt(avatarArea.getX()), juce::roundToInt(avatarArea.getY()),
                              juce::roundToInt(avatarArea.getWidth()), juce::roundToInt(avatarArea.getHeight()),
                              juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
        }
        else
        {
            // Placeholder circle
            g.setColour(APP_COLOR_SURFACE_ALT);
            g.fillRoundedRectangle(avatarArea, 8.0f);
            g.setColour(APP_COLOR_TEXT_MUTED.withAlpha(0.4f));
            g.drawRoundedRectangle(avatarArea.reduced(0.5f), 8.0f, 1.0f);
        }
    }

    // Draw drag-over highlight
    if (isDragOver)
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(APP_COLOR_PRIMARY.withAlpha(0.15f));
        g.fillRoundedRectangle(b, 8.0f);
        g.setColour(APP_COLOR_PRIMARY.withAlpha(0.6f));
        g.drawRoundedRectangle(b.reduced(2.0f), 8.0f, 2.0f);

        g.setColour(APP_COLOR_PRIMARY);
        g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
        g.drawText(TR("voicebank.drop_here"), b, juce::Justification::centred);
    }
}

// ==== Resize ====

void VoicebankPanel::resized()
{
    auto bounds = getLocalBounds().reduced(12);
    const int cardGap = 10;
    const int buttonHeight = 28;
    const int buttonGap = 6;

    // Title
    titleLabel.setBounds(bounds.removeFromTop(20));
    bounds.removeFromTop(6);

    // Button row
    auto buttonRow = bounds.removeFromTop(buttonHeight);
    const int btnWidth = (buttonRow.getWidth() - buttonGap * 2) / 3;
    addButton.setBounds(buttonRow.removeFromLeft(btnWidth));
    buttonRow.removeFromLeft(buttonGap);
    removeButton.setBounds(buttonRow.removeFromLeft(btnWidth));
    buttonRow.removeFromLeft(buttonGap);
    activateButton.setBounds(buttonRow);
    bounds.removeFromTop(cardGap);

    // List card - takes roughly 55% of remaining space
    int listHeight = juce::jmax(120, (bounds.getHeight() - cardGap) * 55 / 100);
    listCardBounds = bounds.removeFromTop(listHeight);
    auto listArea = listCardBounds.reduced(6);
    listBox.setBounds(listArea);
    bounds.removeFromTop(cardGap);

    // Info card - remaining space
    infoCardBounds = bounds;
    auto infoArea = infoCardBounds.reduced(10);
    const int lineHeight = 20;
    const int avatarSize = 64;

    // Top row: avatar on left, name + description on right
    auto topRow = infoArea.removeFromTop(avatarSize + 4);
    avatarBounds = topRow.removeFromLeft(avatarSize);
    topRow.removeFromLeft(8);
    auto nameArea = topRow.removeFromTop(lineHeight + 4);
    infoNameLabel.setBounds(nameArea);
    infoDescriptionLabel.setBounds(topRow);

    infoArea.removeFromTop(4);
    infoTypeLabel.setBounds(infoArea.removeFromTop(lineHeight));
    infoSpeakersLabel.setBounds(infoArea.removeFromTop(lineHeight));
    infoEncoderLabel.setBounds(infoArea.removeFromTop(lineHeight));
    infoSampleRateLabel.setBounds(infoArea.removeFromTop(lineHeight));
    infoHiddenLabel.setBounds(infoArea.removeFromTop(lineHeight));
}

// ==== ListBoxModel ====

int VoicebankPanel::getNumRows()
{
    return static_cast<int>(voicebanks.size());
}

void VoicebankPanel::paintListBoxItem(int row, juce::Graphics& g,
                                       int width, int height, bool isSelected)
{
    if (row < 0 || row >= static_cast<int>(voicebanks.size()))
        return;

    const auto& vb = voicebanks[static_cast<size_t>(row)];
    bool isActive = (row == activeIndex);

    // Background
    if (isSelected)
        g.fillAll(APP_COLOR_PRIMARY.withAlpha(0.15f));
    else if (row % 2 == 1)
        g.fillAll(APP_COLOR_SURFACE_ALT.withAlpha(0.3f));

    if (infoAnimationActive && row == listBox.getSelectedRow())
    {
        const auto pulse = juce::jmap(std::sin(infoAnimationProgress * juce::MathConstants<float>::pi),
                                      0.0f, 1.0f, 0.10f, activationBurst ? 0.30f : 0.20f);
        juce::ColourGradient pulseGradient(APP_COLOR_PRIMARY.withAlpha(pulse), 0.0f, 0.0f,
                                           juce::Colours::white.withAlpha(pulse * 0.7f),
                                           static_cast<float>(width), static_cast<float>(height), false);
        g.setGradientFill(pulseGradient);
        g.fillRoundedRectangle(juce::Rectangle<float>(1.0f, 1.0f, static_cast<float>(width - 2),
                                                      static_cast<float>(height - 2)), 8.0f);
    }

    // Active indicator bar on the left
    if (isActive)
    {
        g.setColour(APP_COLOR_PRIMARY);
        const auto activeWidth = infoAnimationActive && activationBurst
                                     ? static_cast<int>(juce::jmap(std::sin(infoAnimationProgress * juce::MathConstants<float>::pi),
                                                                   3.0f, 7.0f))
                                     : 3;
        g.fillRect(0, 2, activeWidth, height - 4);
    }

    int textX = isActive ? 10 : 6;

    // Avatar thumbnail on the left
    if (vb.avatar.isValid())
    {
        const int thumbSize = height - 8;
        g.drawImageWithin(vb.avatar, textX, 4, thumbSize, thumbSize,
                          juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
        textX += thumbSize + 6;
    }

    // Name (top line)
    g.setColour(isActive ? APP_COLOR_PRIMARY : APP_COLOR_TEXT_PRIMARY);
    g.setFont(juce::Font(juce::FontOptions(14.0f, isActive ? juce::Font::bold : juce::Font::plain)));
    g.drawText(vb.name, textX, 2, width - textX - 6, height / 2,
               juce::Justification::centredLeft, true);

    // Subtitle (bottom line): model type + encoder
    g.setColour(APP_COLOR_TEXT_MUTED);
    g.setFont(juce::Font(juce::FontOptions(11.5f)));
    juce::String subtitle = vb.modelTypeName;
    if (vb.encoder.isNotEmpty())
        subtitle += " | " + vb.encoder;
    if (vb.numSpeakers > 1)
        subtitle += " | " + juce::String(vb.numSpeakers) + " spk";
    g.drawText(subtitle, textX, height / 2, width - textX - 6, height / 2 - 2,
               juce::Justification::centredLeft, true);
}

void VoicebankPanel::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < static_cast<int>(voicebanks.size()))
    {
        updateInfoDisplay();
        startInfoAnimation(false);
    }
}

void VoicebankPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    // Double-click activates the voicebank
    if (row >= 0 && row < static_cast<int>(voicebanks.size()))
    {
        activeIndex = row;
        listBox.repaint();
        updateInfoDisplay();
        startInfoAnimation(true);

        if (onVoicebankActivated)
            onVoicebankActivated(voicebanks[static_cast<size_t>(activeIndex)]);
    }
}

// ==== Public API ====

void VoicebankPanel::loadSfsModel(const juce::File& sfsFile)
{
    VoicebankInfo info;

    if (!parseSfsModelConfig(sfsFile, info))
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            TR("voicebank.error"),
            TR("voicebank.invalid_sfs_model") + "\n" + sfsFile.getFullPathName());
        return;
    }

    // Extract .sfs_model to persistent voicebanks directory
    auto voicebanksDir = PlatformPaths::getVoicebanksDirectory();
    voicebanksDir.createDirectory();

    // Use voicebank name as folder name (sanitize for filesystem)
    juce::String folderName = info.name.isNotEmpty()
        ? info.name : sfsFile.getFileNameWithoutExtension();
    // Remove chars that are problematic in file paths
    folderName = folderName.replaceCharacters("\\/:*?\"<>|", "_________");

    auto targetDir = voicebanksDir.getChildFile(folderName);

    // If already exists, remove old and re-extract
    if (targetDir.isDirectory())
        targetDir.deleteRecursively();

    targetDir.createDirectory();

    // Extract all entries from the ZIP
    juce::ZipFile zip(sfsFile);
    for (int i = 0; i < zip.getNumEntries(); ++i)
    {
        auto result = zip.uncompressEntry(i, targetDir);
        if (result.failed())
        {
            DBG("VoicebankPanel: Failed to extract entry " + juce::String(i)
                + " from " + sfsFile.getFileName() + ": " + result.getErrorMessage());
        }
    }

    // Verify config.json was extracted
    if (!targetDir.getChildFile("config.json").existsAsFile())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            TR("voicebank.error"),
            "Failed to extract voicebank to:\n" + targetDir.getFullPathName());
        targetDir.deleteRecursively();
        return;
    }

    // Check for duplicate (same name already in list)
    for (auto it = voicebanks.begin(); it != voicebanks.end(); ++it)
    {
        if (it->name == info.name)
        {
            // Replace existing entry
            if (activeIndex == static_cast<int>(std::distance(voicebanks.begin(), it)))
                activeIndex = -1;
            voicebanks.erase(it);
            break;
        }
    }

    info.path = targetDir.getFullPathName();
    info.loaded = true;
    voicebanks.push_back(std::move(info));
    listBox.updateContent();

    if (voicebanks.size() == 1)
    {
        activeIndex = 0;
        listBox.selectRow(0);
        updateInfoDisplay();
        startInfoAnimation(true);
    }

    listBox.repaint();

    DBG("VoicebankPanel: Extracted voicebank '" + folderName + "' to " + targetDir.getFullPathName());
}

void VoicebankPanel::addVoicebank(const juce::File& directory)
{
    VoicebankInfo info;
    info.name = directory.getFileName();
    info.path = directory.getFullPathName();

    scanDirectory(directory, info);

    if (info.modelTypeIndex < 0)
    {
        // Could not detect model type - show error
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            TR("voicebank.error"),
            TR("voicebank.no_model_found") + "\n" + directory.getFullPathName());
        return;
    }

    info.loaded = true;
    info.modelTypeName = getModelTypeName(info.modelTypeIndex);
    voicebanks.push_back(std::move(info));
    listBox.updateContent();

    // Auto-activate if it's the first voicebank
    if (voicebanks.size() == 1)
    {
        activeIndex = 0;
        listBox.selectRow(0);
        updateInfoDisplay();
        startInfoAnimation(true);
    }

    listBox.repaint();
}

void VoicebankPanel::removeSelectedVoicebank()
{
    int selected = listBox.getSelectedRow();
    if (selected < 0 || selected >= static_cast<int>(voicebanks.size()))
        return;

    // Delete the persistent voicebank directory
    auto& vb = voicebanks[static_cast<size_t>(selected)];
    juce::File vbDir(vb.path);
    if (vbDir.isDirectory())
    {
        // Check if it's inside the voicebanks directory before deleting
        auto voicebanksDir = PlatformPaths::getVoicebanksDirectory();
        if (vbDir.isAChildOf(voicebanksDir))
            vbDir.deleteRecursively();
    }

    // Adjust active index
    if (selected == activeIndex)
        activeIndex = -1;
    else if (selected < activeIndex)
        --activeIndex;

    voicebanks.erase(voicebanks.begin() + selected);
    listBox.updateContent();
    listBox.deselectAllRows();
    updateInfoDisplay();

    if (onVoicebankRemoved)
        onVoicebankRemoved();
}

const VoicebankPanel::VoicebankInfo* VoicebankPanel::getSelectedVoicebank() const
{
    int selected = listBox.getSelectedRow();
    if (selected >= 0 && selected < static_cast<int>(voicebanks.size()))
        return &voicebanks[static_cast<size_t>(selected)];
    return nullptr;
}

const VoicebankPanel::VoicebankInfo* VoicebankPanel::getActiveVoicebank() const
{
    if (activeIndex >= 0 && activeIndex < static_cast<int>(voicebanks.size()))
        return &voicebanks[static_cast<size_t>(activeIndex)];
    return nullptr;
}

bool VoicebankPanel::setActiveVoicebankByPath(const juce::File& path)
{
    const auto targetPath = path.getFullPathName();
    for (int i = 0; i < static_cast<int>(voicebanks.size()); ++i)
    {
        const auto itemPath = juce::File(voicebanks[static_cast<size_t>(i)].path)
                                  .getFullPathName();
        if (itemPath.equalsIgnoreCase(targetPath))
        {
            activeIndex = i;
            listBox.selectRow(i, juce::dontSendNotification);
            listBox.repaint();
            updateInfoDisplay();
            startInfoAnimation(true);
            return true;
        }
    }
    return false;
}

// ==== Private Helpers ====

bool VoicebankPanel::parseSfsModelConfig(const juce::File& sfsFile, VoicebankInfo& info)
{
    // .sfs_model is a ZIP file; read config.json from it
    juce::ZipFile zip(sfsFile);
    if (zip.getNumEntries() == 0)
        return false;

    auto idx = zip.getIndexOfFileName("config.json");
    if (idx < 0)
        return false;

    std::unique_ptr<juce::InputStream> stream(zip.createStreamForEntry(idx));
    if (stream == nullptr)
        return false;

    auto jsonStr = stream->readEntireStreamAsString();
    auto parsed = juce::JSON::parse(jsonStr);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr)
        return false;

    info.name = obj->getProperty("name").toString();
    info.description = obj->getProperty("description").toString();
    info.modelTypeIndex = static_cast<int>(obj->getProperty("model_type_index"));
    info.modelTypeName = obj->getProperty("model_type_name").toString();
    info.encoder = obj->getProperty("encoder").toString();
    info.sampleRate = static_cast<int>(obj->getProperty("sample_rate"));
    info.blockSize = static_cast<int>(obj->getProperty("block_size"));
    info.numSpeakers = static_cast<int>(obj->getProperty("n_spk"));
    info.nHidden = static_cast<int>(obj->getProperty("n_hidden"));
    info.velocityTType = obj->getProperty("velocity_t_type").toString();

    if (obj->hasProperty("speaker_names"))
    {
        if (auto* arr = obj->getProperty("speaker_names").getArray())
        {
            for (const auto& item : *arr)
                info.speakerNames.add(item.toString());
        }
    }

    // Load avatar.png from ZIP if present
    auto avatarIdx = zip.getIndexOfFileName("avatar.png");
    if (avatarIdx >= 0)
    {
        std::unique_ptr<juce::InputStream> avatarStream(zip.createStreamForEntry(avatarIdx));
        if (avatarStream)
            info.avatar = juce::ImageFileFormat::loadFrom(*avatarStream);
    }

    if (info.name.isEmpty())
        info.name = sfsFile.getFileNameWithoutExtension();
    if (info.modelTypeName.isEmpty())
        info.modelTypeName = getModelTypeName(info.modelTypeIndex);

    return info.modelTypeIndex >= 0;
}

// ==== FileDragAndDropTarget ====

bool VoicebankPanel::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase(".sfs_model"))
            return true;
    return false;
}

void VoicebankPanel::filesDropped(const juce::StringArray& files, int, int)
{
    isDragOver = false;
    repaint();

    for (const auto& f : files)
    {
        juce::File file(f);
        if (file.hasFileExtension("sfs_model") && file.existsAsFile())
            loadSfsModel(file);
    }
}

void VoicebankPanel::fileDragEnter(const juce::StringArray&, int, int)
{
    isDragOver = true;
    repaint();
}

void VoicebankPanel::fileDragExit(const juce::StringArray&)
{
    isDragOver = false;
    repaint();
}

void VoicebankPanel::scanDirectory(const juce::File& dir, VoicebankInfo& info)
{
    // Detect model type by looking for characteristic ONNX files
    bool hasEncoder = dir.getChildFile("encoder.onnx").existsAsFile();
    bool hasVelocity = dir.getChildFile("velocity.onnx").existsAsFile();
    bool hasSovits = dir.getChildFile("sovits.onnx").existsAsFile();
    bool hasRVC = dir.getChildFile("rvc.onnx").existsAsFile();

    // Try to read config.yaml or config.json for model metadata
    auto configYaml = dir.getChildFile("config.yaml");
    auto configJson = dir.getChildFile("config.json");

    if (hasRVC)
    {
        info.modelTypeIndex = 5;
    }
    else if (hasSovits)
    {
        // So-VITS-SVC (type 2) - single sovits.onnx file
        info.modelTypeIndex = 2;
    }
    else if (hasEncoder && hasVelocity)
    {
        // Could be DDSP 6.0/6.1/6.3 or Reflow-VAE
        // Try to determine from config files
        info.modelTypeIndex = 4; // Default to DDSP 6.3

        if (configYaml.existsAsFile())
        {
            auto yamlContent = configYaml.loadFileAsString();
            if (yamlContent.contains("reflow") || yamlContent.contains("Reflow")
                || yamlContent.contains("vae") || yamlContent.contains("VAE"))
            {
                info.modelTypeIndex = 1; // Reflow-VAE-SVC
            }
            else if (yamlContent.contains("6.0") || yamlContent.contains("NaiveV2Diff"))
            {
                info.modelTypeIndex = 0; // DDSP 6.0
            }
            else if (yamlContent.contains("6.1") || yamlContent.contains("spk_mix"))
            {
                info.modelTypeIndex = 3; // DDSP 6.1
            }
        }
    }
    else
    {
        info.modelTypeIndex = -1; // Unknown
        return;
    }

    // Parse config.yaml for metadata (simple key:value parsing)
    if (configYaml.existsAsFile())
    {
        auto content = configYaml.loadFileAsString();
        auto lines = juce::StringArray::fromLines(content);

        for (const auto& line : lines)
        {
            auto trimmed = line.trim();

            if (trimmed.startsWith("encoder:"))
                info.encoder = trimmed.fromFirstOccurrenceOf(":", false, false).trim().unquoted();
            else if (trimmed.startsWith("n_spk:"))
                info.numSpeakers = trimmed.fromFirstOccurrenceOf(":", false, false).trim().getIntValue();
            else if (trimmed.startsWith("sample_rate:") || trimmed.startsWith("sampling_rate:"))
                info.sampleRate = trimmed.fromFirstOccurrenceOf(":", false, false).trim().getIntValue();
            else if (trimmed.startsWith("block_size:"))
                info.blockSize = trimmed.fromFirstOccurrenceOf(":", false, false).trim().getIntValue();
        }
    }

    // Parse config.json as fallback (So-VITS-SVC uses JSON config)
    if (configJson.existsAsFile())
    {
        auto jsonContent = configJson.loadFileAsString();
        auto parsed = juce::JSON::parse(jsonContent);

        if (auto* obj = parsed.getDynamicObject())
        {
            if (obj->hasProperty("model"))
            {
                auto model = obj->getProperty("model");
                if (auto* modelObj = model.getDynamicObject())
                {
                    if (modelObj->hasProperty("speech_encoder"))
                        info.encoder = modelObj->getProperty("speech_encoder").toString();
                    if (modelObj->hasProperty("n_speakers"))
                        info.numSpeakers = static_cast<int>(modelObj->getProperty("n_speakers"));
                }
            }

            if (obj->hasProperty("data"))
            {
                auto data = obj->getProperty("data");
                if (auto* dataObj = data.getDynamicObject())
                {
                    if (dataObj->hasProperty("sampling_rate"))
                        info.sampleRate = static_cast<int>(dataObj->getProperty("sampling_rate"));
                }
            }
        }
    }

    // Read speaker names if spk_info.json exists
    auto spkInfo = dir.getChildFile("spk_info.json");
    if (spkInfo.existsAsFile())
    {
        auto spkContent = spkInfo.loadFileAsString();
        auto spkParsed = juce::JSON::parse(spkContent);
        if (auto* spkObj = spkParsed.getDynamicObject())
        {
            for (auto& prop : spkObj->getProperties())
                info.speakerNames.add(prop.name.toString());
        }
    }
}

void VoicebankPanel::updateInfoDisplay()
{
    const VoicebankInfo* info = nullptr;

    // Show info for selected item, or active item, or nothing
    int selected = listBox.getSelectedRow();
    if (selected >= 0 && selected < static_cast<int>(voicebanks.size()))
        info = &voicebanks[static_cast<size_t>(selected)];
    else if (activeIndex >= 0 && activeIndex < static_cast<int>(voicebanks.size()))
        info = &voicebanks[static_cast<size_t>(activeIndex)];

    if (info)
    {
        infoNameLabel.setText(info->name, juce::dontSendNotification);
        infoDescriptionLabel.setText(
            info->description.isNotEmpty() ? info->description : "",
            juce::dontSendNotification);
        avatarImage = info->avatar;
        infoTypeLabel.setText(TR("voicebank.type") + ": " + info->modelTypeName,
                             juce::dontSendNotification);
        infoSpeakersLabel.setText(TR("voicebank.speakers") + ": " + juce::String(info->numSpeakers),
                                  juce::dontSendNotification);
        infoEncoderLabel.setText(TR("voicebank.encoder_label") + ": " + info->encoder,
                                 juce::dontSendNotification);
        infoSampleRateLabel.setText(TR("voicebank.sample_rate") + ": "
                                     + juce::String(info->sampleRate) + " Hz",
                                    juce::dontSendNotification);
        infoHiddenLabel.setText(TR("voicebank.n_hidden") + ": " + juce::String(info->nHidden),
                                juce::dontSendNotification);
    }
    else
    {
        infoNameLabel.setText(TR("voicebank.no_selection"), juce::dontSendNotification);
        infoDescriptionLabel.setText("", juce::dontSendNotification);
        avatarImage = {};
        infoTypeLabel.setText("", juce::dontSendNotification);
        infoSpeakersLabel.setText("", juce::dontSendNotification);
        infoEncoderLabel.setText("", juce::dontSendNotification);
        infoSampleRateLabel.setText("", juce::dontSendNotification);
        infoHiddenLabel.setText("", juce::dontSendNotification);
    }

    displayedInfoIndex = info != nullptr ? (selected >= 0 ? selected : activeIndex) : -1;

    applyInfoAnimationState();

    repaint(); // repaint for avatar
}

void VoicebankPanel::timerCallback()
{
    if (!infoAnimationActive)
        return;

    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - infoAnimationStartTimeMs;
    const auto t = juce::jlimit(0.0, 1.0, elapsedMs / static_cast<double>(infoAnimationDurationMs));
    infoAnimationProgress = static_cast<float>(t * t * (3.0 - 2.0 * t));
    applyInfoAnimationState();
    listBox.repaint();
    repaint();

    if (t >= 1.0)
    {
        infoAnimationActive = false;
        activationBurst = false;
        infoAnimationProgress = 1.0f;
        applyInfoAnimationState();
        stopTimer();
        listBox.repaint();
        repaint();
    }
}

void VoicebankPanel::startInfoAnimation(bool emphasizeActivation)
{
    if (displayedInfoIndex < 0)
        return;

    activationBurst = emphasizeActivation;
    infoAnimationActive = true;
    infoAnimationProgress = 0.0f;
    infoAnimationStartTimeMs = juce::Time::getMillisecondCounterHiRes();
    applyInfoAnimationState();
    startTimerHz(60);
}

void VoicebankPanel::applyInfoAnimationState()
{
    const auto progress = infoAnimationActive ? infoAnimationProgress : 1.0f;
    const auto baseOffset = juce::jmap(progress, 18.0f, 0.0f);
    const auto descriptionOffset = juce::jmap(progress, 24.0f, 0.0f);
    const auto alpha = progress;

    infoNameLabel.setAlpha(alpha);
    infoNameLabel.setTransform(juce::AffineTransform::translation(baseOffset, 0.0f));

    infoDescriptionLabel.setAlpha(alpha);
    infoDescriptionLabel.setTransform(juce::AffineTransform::translation(descriptionOffset, 0.0f));

    for (auto* label : { &infoTypeLabel, &infoSpeakersLabel, &infoEncoderLabel,
                         &infoSampleRateLabel, &infoHiddenLabel })
    {
        label->setAlpha(alpha);
        label->setTransform(juce::AffineTransform::translation(baseOffset * 0.75f, 0.0f));
    }

    const auto buttonBounce = activationBurst && infoAnimationActive
                                  ? 1.0f + 0.10f * std::sin(progress * juce::MathConstants<float>::pi)
                                  : 1.0f;
    activateButton.setTransform(juce::AffineTransform::scale(buttonBounce, buttonBounce,
        static_cast<float>(activateButton.getBounds().getCentreX()),
        static_cast<float>(activateButton.getBounds().getCentreY())));
}

int VoicebankPanel::getDisplayInfoIndex() const
{
    const auto selected = listBox.getSelectedRow();
    if (selected >= 0 && selected < static_cast<int>(voicebanks.size()))
        return selected;

    if (activeIndex >= 0 && activeIndex < static_cast<int>(voicebanks.size()))
        return activeIndex;

    return -1;
}

juce::String VoicebankPanel::getModelTypeName(int typeIndex) const
{
    switch (typeIndex)
    {
        case 0: return "DDSP-SVC 6.0";
        case 1: return "Reflow-VAE-SVC";
        case 2: return "So-VITS-SVC";
        case 3: return "DDSP-SVC 6.1";
        case 4: return "DDSP-SVC 6.3";
        case 5: return "RVC";
        default: return TR("voicebank.unknown");
    }
}

void VoicebankPanel::scanVoicebanksDirectory()
{
    auto voicebanksDir = PlatformPaths::getVoicebanksDirectory();
    if (!voicebanksDir.isDirectory())
    {
        DBG("VoicebankPanel: voicebanks directory does not exist: " + voicebanksDir.getFullPathName());
        return;
    }

    DBG("VoicebankPanel: Scanning voicebanks directory: " + voicebanksDir.getFullPathName());

    // Keep track of existing paths to avoid duplicates
    std::set<juce::String> existingPaths;
    for (const auto& vb : voicebanks)
        existingPaths.insert(vb.path);

    auto subDirs = voicebanksDir.findChildFiles(
        juce::File::findDirectories, false);

    for (const auto& subDir : subDirs)
    {
        // Skip if already in the list
        if (existingPaths.count(subDir.getFullPathName()) > 0)
            continue;

        auto configFile = subDir.getChildFile("config.json");
        if (!configFile.existsAsFile())
            continue;

        // Parse config.json directly from the directory
        auto jsonStr = configFile.loadFileAsString();
        auto parsed = juce::JSON::parse(jsonStr);
        auto* obj = parsed.getDynamicObject();
        if (!obj)
            continue;

        VoicebankInfo info;
        info.path = subDir.getFullPathName();
        info.name = obj->getProperty("name").toString();
        info.description = obj->getProperty("description").toString();
        info.modelTypeIndex = static_cast<int>(obj->getProperty("model_type_index"));
        info.modelTypeName = obj->getProperty("model_type_name").toString();
        info.encoder = obj->getProperty("encoder").toString();
        info.sampleRate = static_cast<int>(obj->getProperty("sample_rate"));
        info.blockSize = static_cast<int>(obj->getProperty("block_size"));
        info.numSpeakers = static_cast<int>(obj->getProperty("n_spk"));
        info.nHidden = static_cast<int>(obj->getProperty("n_hidden"));
        info.velocityTType = obj->getProperty("velocity_t_type").toString();

        if (obj->hasProperty("speaker_names"))
        {
            if (auto* arr = obj->getProperty("speaker_names").getArray())
                for (const auto& item : *arr)
                    info.speakerNames.add(item.toString());
        }

        // Load avatar.png from directory if present
        auto avatarFile = subDir.getChildFile("avatar.png");
        if (avatarFile.existsAsFile())
            info.avatar = juce::ImageFileFormat::loadFrom(avatarFile);

        if (info.name.isEmpty())
            info.name = subDir.getFileName();
        if (info.modelTypeName.isEmpty())
            info.modelTypeName = getModelTypeName(info.modelTypeIndex);

        if (info.modelTypeIndex >= 0)
        {
            info.loaded = true;
            voicebanks.push_back(std::move(info));
            DBG("VoicebankPanel: Found voicebank: " + subDir.getFileName());
        }
    }

    listBox.updateContent();
    listBox.repaint();
    updateInfoDisplay();

    DBG("VoicebankPanel: Total voicebanks: " + juce::String(voicebanks.size()));
}
