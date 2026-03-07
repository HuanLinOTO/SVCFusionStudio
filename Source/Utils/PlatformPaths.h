#pragma once

#include "../JuceHeader.h"

/**
 * Platform-specific path utilities.
 *
 * macOS:
 *   - Models: App.app/Contents/Resources/models/
 *   - Logs: ~/Library/Logs/SVCFusion Studio/
 *   - Config: ~/Library/Application Support/SVCFusion Studio/
 *
 * Windows:
 *   - Models: <exe_dir>/models/
 *   - Logs: %APPDATA%/SVCFusion Studio/Logs/
 *   - Config: %APPDATA%/SVCFusion Studio/
 *
 * Linux:
 *   - Models: <exe_dir>/models/
 *   - Logs: ~/.config/SVCFusion Studio/logs/
 *   - Config: ~/.config/SVCFusion Studio/
 */
namespace PlatformPaths
{
    inline juce::File getModelsDirectory()
    {
    #if JUCE_MAC
        // macOS: Use Resources folder inside app bundle
        auto appBundle = juce::File::getSpecialLocation(juce::File::currentApplicationFile);
        return appBundle.getChildFile("Contents/Resources/models");
    #else
        // Windows/Linux: Use models folder next to executable
        return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                   .getParentDirectory()
                   .getChildFile("models");
    #endif
    }

    inline juce::File getModelFile(const juce::String& fileName)
    {
        auto probe = getModelsDirectory().getChildFile(fileName);
        if (probe.existsAsFile())
            return probe;

        // Development fallback: <repo>/Resources/models/
        auto cwdProbe = juce::File::getCurrentWorkingDirectory()
                            .getChildFile("Resources/models")
                            .getChildFile(fileName);
        if (cwdProbe.existsAsFile())
            return cwdProbe;

        // Walk up from executable directory and probe both:
        //   <dir>/models/<file>
        //   <dir>/Resources/models/<file>
        auto dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                       .getParentDirectory();
        for (int i = 0; i < 8 && dir.exists(); ++i)
        {
            auto modelsCandidate = dir.getChildFile("models").getChildFile(fileName);
            if (modelsCandidate.existsAsFile())
                return modelsCandidate;

            auto resourcesCandidate = dir.getChildFile("Resources/models").getChildFile(fileName);
            if (resourcesCandidate.existsAsFile())
                return resourcesCandidate;

            auto parent = dir.getParentDirectory();
            if (parent == dir)
                break;
            dir = parent;
        }

        // Default path used in production packaging.
        return probe;
    }

    /**
     * Voicebanks directory: stores extracted .sfs_model voicebanks permanently.
     * Each voicebank is a sub-folder containing config.json + ONNX files.
     *
     * macOS:   App.app/Contents/Resources/voicebanks/
     * Windows: <exe_dir>/voicebanks/
     * Linux:   <exe_dir>/voicebanks/
     */
    inline juce::File getVoicebanksDirectory()
    {
    #if JUCE_MAC
        auto appBundle = juce::File::getSpecialLocation(juce::File::currentApplicationFile);
        return appBundle.getChildFile("Contents/Resources/voicebanks");
    #else
        return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                   .getParentDirectory()
                   .getChildFile("voicebanks");
    #endif
    }

    inline juce::File getLogsDirectory()
    {
    #if JUCE_MAC
        // macOS: ~/Library/Logs/SVCFusion Studio/
        return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                   .getChildFile("Library/Logs/SVCFusion Studio");
    #elif JUCE_WINDOWS
        // Windows: %APPDATA%/SVCFusion Studio/Logs/
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("SVCFusion Studio/Logs");
    #else
        // Linux: ~/.config/SVCFusion Studio/logs/
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("SVCFusion Studio/logs");
    #endif
    }

    inline juce::File getConfigDirectory()
    {
        // All platforms use userApplicationDataDirectory
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("SVCFusion Studio");
    }

    inline juce::File getLogFile(const juce::String& name)
    {
        auto logsDir = getLogsDirectory();
        logsDir.createDirectory();
        return logsDir.getChildFile(name);
    }

    inline juce::File getConfigFile(const juce::String& name)
    {
        auto configDir = getConfigDirectory();
        configDir.createDirectory();
        return configDir.getChildFile(name);
    }
}
