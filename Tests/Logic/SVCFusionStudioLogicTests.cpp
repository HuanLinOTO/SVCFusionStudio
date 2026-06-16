#include "../../Source/Models/Project.h"
#include "../../Source/Audio/SVCInferenceEngine.h"
#include "../../Source/Audio/SVCModelSession.h"
#include "../../Source/UI/PianoRoll/NoteSplitter.h"
#include "../../Source/Utils/Constants.h"
#include "../../Source/Utils/CurveResampler.h"
#include "../../Source/Utils/HNSepCurveProcessor.h"
#include "../../Source/Utils/OnnxRuntimeLoader.h"

#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string &message) {
  if (!condition)
    throw TestFailure(message);
}

template <typename T>
void expectEqual(const T &actual, const T &expected, const std::string &label) {
  if (actual == expected)
    return;

  std::ostringstream stream;
  stream << label << " expected=" << expected << " actual=" << actual;
  throw TestFailure(stream.str());
}

void expectNear(float actual, float expected, float epsilon,
                const std::string &label) {
  if (std::abs(actual - expected) <= epsilon)
    return;

  std::ostringstream stream;
  stream << label << " expected=" << expected << " actual=" << actual;
  throw TestFailure(stream.str());
}

void expectFloatVector(const std::vector<float> &actual,
                       const std::vector<float> &expected,
                       const std::string &label, float epsilon = 1.0e-5f) {
  expectEqual(actual.size(), expected.size(), label + " size");
  for (size_t index = 0; index < actual.size(); ++index) {
    expectNear(actual[index], expected[index], epsilon,
               label + "[" + std::to_string(index) + "]");
  }
}

void expectMelClip(const std::vector<std::vector<float>> &actual,
                   const std::vector<std::vector<float>> &expected,
                   const std::string &label) {
  expectEqual(actual.size(), expected.size(), label + " frames");
  for (size_t frame = 0; frame < actual.size(); ++frame) {
    expectFloatVector(actual[frame], expected[frame],
                      label + " frame " + std::to_string(frame));
  }
}

std::vector<float> makeLinearSequence(int count, float start, float step = 1.0f) {
  std::vector<float> values(static_cast<size_t>(count));
  for (int index = 0; index < count; ++index) {
    values[static_cast<size_t>(index)] = start + step * static_cast<float>(index);
  }
  return values;
}

std::vector<std::vector<float>> makeMelSequence(int frames, int bins) {
  std::vector<std::vector<float>> mel(static_cast<size_t>(frames),
                                      std::vector<float>(static_cast<size_t>(bins), 0.0f));
  for (int frame = 0; frame < frames; ++frame) {
    for (int bin = 0; bin < bins; ++bin) {
      mel[static_cast<size_t>(frame)][static_cast<size_t>(bin)] =
          static_cast<float>(frame * 10 + bin);
    }
  }
  return mel;
}

std::vector<float> subVector(const std::vector<float> &source, size_t offset,
                             size_t count) {
  return std::vector<float>(source.begin() + static_cast<ptrdiff_t>(offset),
                            source.begin() + static_cast<ptrdiff_t>(offset + count));
}

std::vector<std::vector<float>> subMel(const std::vector<std::vector<float>> &source,
                                       size_t offset, size_t count) {
  return std::vector<std::vector<float>>(
      source.begin() + static_cast<ptrdiff_t>(offset),
      source.begin() + static_cast<ptrdiff_t>(offset + count));
}

void expectNoteMatches(const Note &actual, const Note &expected,
                       const std::string &label) {
  expectEqual(actual.getStartFrame(), expected.getStartFrame(),
              label + " startFrame");
  expectEqual(actual.getEndFrame(), expected.getEndFrame(),
              label + " endFrame");
  expectEqual(actual.getSrcStartFrame(), expected.getSrcStartFrame(),
              label + " srcStartFrame");
  expectEqual(actual.getSrcEndFrame(), expected.getSrcEndFrame(),
              label + " srcEndFrame");
  expectNear(actual.getMidiNote(), expected.getMidiNote(), 1.0e-5f,
             label + " midiNote");
  expectFloatVector(actual.getClipWaveform(), expected.getClipWaveform(),
                    label + " clipWaveform");
  expectFloatVector(actual.getClipHarmonicWaveform(),
                    expected.getClipHarmonicWaveform(),
                    label + " clipHarmonicWaveform");
  expectFloatVector(actual.getClipNoiseWaveform(),
                    expected.getClipNoiseWaveform(),
                    label + " clipNoiseWaveform");
  expectFloatVector(actual.getVoicingCurve(), expected.getVoicingCurve(),
                    label + " voicingCurve");
  expectFloatVector(actual.getBreathCurve(), expected.getBreathCurve(),
                    label + " breathCurve");
  expectFloatVector(actual.getTensionCurve(), expected.getTensionCurve(),
                    label + " tensionCurve");
  expectMelClip(actual.getClipMel(), expected.getClipMel(), label + " clipMel");
}

void testSplitPreservesHNSepStateAndUndoRedo() {
  Project project;
  PitchUndoManager undoManager;
  NoteSplitter splitter;

  splitter.setProject(&project);
  splitter.setUndoManager(&undoManager);

  Note originalNote(10, 26, 60.0f);
  originalNote.setSrcStartFrame(20);
  originalNote.setSrcEndFrame(36);
  originalNote.setClipWaveform(makeLinearSequence(16 * HOP_SIZE, 0.0f));
  originalNote.setClipHarmonicWaveform(makeLinearSequence(16 * HOP_SIZE, 1000.0f));
  originalNote.setClipNoiseWaveform(makeLinearSequence(16 * HOP_SIZE, 2000.0f));
  originalNote.setClipMel(makeMelSequence(16, 3));
  originalNote.setVoicingCurve(makeLinearSequence(16, 10.0f));
  originalNote.setBreathCurve(makeLinearSequence(16, 30.0f));
  originalNote.setTensionCurve(makeLinearSequence(16, -20.0f, 2.0f));

  project.addNote(originalNote);

  const bool didSplit = splitter.splitNoteAtFrame(&project.getNotes().front(), 18);
  expect(didSplit, "split should succeed for a note long enough to split");
  expectEqual(project.getNotes().size(), static_cast<size_t>(2),
              "split note count");

  const auto &left = project.getNotes()[0];
  const auto &right = project.getNotes()[1];

  expectEqual(left.getStartFrame(), 10, "left startFrame");
  expectEqual(left.getEndFrame(), 18, "left endFrame");
  expectEqual(left.getSrcStartFrame(), 20, "left srcStartFrame");
  expectEqual(left.getSrcEndFrame(), 28, "left srcEndFrame");
  expectEqual(right.getStartFrame(), 18, "right startFrame");
  expectEqual(right.getEndFrame(), 26, "right endFrame");
  expectEqual(right.getSrcStartFrame(), 28, "right srcStartFrame");
  expectEqual(right.getSrcEndFrame(), 36, "right srcEndFrame");

  expectFloatVector(left.getClipWaveform(),
                    subVector(originalNote.getClipWaveform(), 0, 8 * HOP_SIZE),
                    "left clipWaveform");
  expectFloatVector(right.getClipWaveform(),
                    subVector(originalNote.getClipWaveform(), 8 * HOP_SIZE,
                              8 * HOP_SIZE),
                    "right clipWaveform");
  expectFloatVector(left.getClipHarmonicWaveform(),
                    subVector(originalNote.getClipHarmonicWaveform(), 0,
                              8 * HOP_SIZE),
                    "left clipHarmonicWaveform");
  expectFloatVector(right.getClipHarmonicWaveform(),
                    subVector(originalNote.getClipHarmonicWaveform(),
                              8 * HOP_SIZE, 8 * HOP_SIZE),
                    "right clipHarmonicWaveform");
  expectFloatVector(left.getClipNoiseWaveform(),
                    subVector(originalNote.getClipNoiseWaveform(), 0,
                              8 * HOP_SIZE),
                    "left clipNoiseWaveform");
  expectFloatVector(right.getClipNoiseWaveform(),
                    subVector(originalNote.getClipNoiseWaveform(),
                              8 * HOP_SIZE, 8 * HOP_SIZE),
                    "right clipNoiseWaveform");
  expectMelClip(left.getClipMel(), subMel(originalNote.getClipMel(), 0, 8),
                "left clipMel");
  expectMelClip(right.getClipMel(), subMel(originalNote.getClipMel(), 8, 8),
                "right clipMel");
  expectFloatVector(left.getVoicingCurve(),
                    subVector(originalNote.getVoicingCurve(), 0, 8),
                    "left voicingCurve");
  expectFloatVector(right.getVoicingCurve(),
                    subVector(originalNote.getVoicingCurve(), 8, 8),
                    "right voicingCurve");
  expectFloatVector(left.getBreathCurve(),
                    subVector(originalNote.getBreathCurve(), 0, 8),
                    "left breathCurve");
  expectFloatVector(right.getBreathCurve(),
                    subVector(originalNote.getBreathCurve(), 8, 8),
                    "right breathCurve");
  expectFloatVector(left.getTensionCurve(),
                    subVector(originalNote.getTensionCurve(), 0, 8),
                    "left tensionCurve");
  expectFloatVector(right.getTensionCurve(),
                    subVector(originalNote.getTensionCurve(), 8, 8),
                    "right tensionCurve");

  expect(undoManager.canUndo(), "split should create an undo action");
  undoManager.undo();

  expectEqual(project.getNotes().size(), static_cast<size_t>(1),
              "undo note count");
  expectNoteMatches(project.getNotes().front(), originalNote, "undo restored note");

  expect(undoManager.canRedo(), "undo should create a redo action");
  undoManager.redo();

  expectEqual(project.getNotes().size(), static_cast<size_t>(2),
              "redo note count");
  expectEqual(project.getNotes()[0].getSrcEndFrame(), 28, "redo left srcEndFrame");
  expectEqual(project.getNotes()[1].getSrcStartFrame(), 28,
              "redo right srcStartFrame");
}

void testStretchRebuildResamplesHNSepMasterCurves() {
  Project project;
  auto &audioData = project.getAudioData();
  audioData.melSpectrogram.assign(40, std::vector<float>(1, 0.0f));

  const std::vector<float> voicing = {100.0f, 90.0f, 80.0f, 70.0f, 60.0f, 50.0f};
  const std::vector<float> breath = {50.0f, 55.0f, 60.0f, 65.0f, 70.0f, 75.0f};
  const std::vector<float> tension = {-20.0f, -10.0f, 0.0f, 10.0f, 20.0f, 30.0f};

  Note note(10, 16, 60.0f);
  note.setVoicingCurve(voicing);
  note.setBreathCurve(breath);
  note.setTensionCurve(tension);
  project.addNote(note);

  HNSepCurveProcessor::rebuildCurvesFromNotes(project);
  expectFloatVector(subVector(audioData.voicingCurve, 10, 6), voicing,
                    "initial master voicing");
  expectFloatVector(subVector(audioData.breathCurve, 10, 6), breath,
                    "initial master breath");
  expectFloatVector(subVector(audioData.tensionCurve, 10, 6), tension,
                    "initial master tension");

  project.getNotes()[0].setEndFrame(19);
  HNSepCurveProcessor::rebuildCurvesFromNotes(project);

  const auto expectedVoicing = CurveResampler::resampleLinear(voicing, 9);
  const auto expectedBreath = CurveResampler::resampleLinear(breath, 9);
  const auto expectedTension = CurveResampler::resampleLinear(tension, 9);

  expectFloatVector(subVector(audioData.voicingCurve, 10, 9), expectedVoicing,
                    "stretched master voicing");
  expectFloatVector(subVector(audioData.breathCurve, 10, 9), expectedBreath,
                    "stretched master breath");
  expectFloatVector(subVector(audioData.tensionCurve, 10, 9), expectedTension,
                    "stretched master tension");

  expectFloatVector(project.getNotes()[0].getVoicingCurve(), voicing,
                    "note-local voicing remains unchanged");
  expectFloatVector(project.getNotes()[0].getBreathCurve(), breath,
                    "note-local breath remains unchanged");
  expectFloatVector(project.getNotes()[0].getTensionCurve(), tension,
                    "note-local tension remains unchanged");

  project.getNotes()[0].setEndFrame(16);
  HNSepCurveProcessor::rebuildCurvesFromNotes(project);
  expectFloatVector(subVector(audioData.voicingCurve, 10, 6), voicing,
                    "restored master voicing");
}

std::string getEnvValue(const char *name) {
#if defined(_WIN32)
  char *value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
    return {};
  std::string result(value);
  std::free(value);
  return result;
#else
  const char *value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
#endif
}

void maybeRunRVCIntegrationTest() {
  const auto modelDirEnv = getEnvValue("SVCFUSIONSTUDIO_RVC_TEST_MODEL_DIR");
  const auto contentVecEnv = getEnvValue("SVCFUSIONSTUDIO_RVC_TEST_CONTENTVEC");
  const auto outEnv = getEnvValue("SVCFUSIONSTUDIO_RVC_TEST_OUTPUT");
  if (modelDirEnv.empty() || contentVecEnv.empty() || outEnv.empty())
    return;

  expect(OnnxRuntimeLoader::ensureLoaded(), "ONNX Runtime should load for RVC integration test");

  SVCInferenceEngine engine;
  SVCModelSession session;
  expect(engine.loadContentVec(juce::File(contentVecEnv), "CPU", 0),
         "ContentVec should load for RVC integration test");
  expect(session.loadFromDirectory(juce::File(modelDirEnv), "CPU", 0),
         "RVC model directory should load");

  constexpr int sampleRate = SAMPLE_RATE;
  const int numSamples = sampleRate / 2;
  std::vector<float> audio(static_cast<size_t>(numSamples), 0.0f);
  for (int i = 0; i < numSamples; ++i) {
    const double phase = 2.0 * 3.14159265358979323846 * 220.0
                       * static_cast<double>(i) / static_cast<double>(sampleRate);
    audio[static_cast<size_t>(i)] = 0.08f * static_cast<float>(std::sin(phase));
  }

  const int f0Frames = (numSamples + HOP_SIZE - 1) / HOP_SIZE;
  std::vector<float> f0(static_cast<size_t>(f0Frames), 220.0f);
  auto out = engine.inferRVC(session, audio.data(), numSamples, sampleRate, f0,
                             SVCInferenceParams{});
  expect(!out.empty(), "RVC integration output should not be empty");
  expectEqual(out.size(), audio.size(), "RVC integration output size");

  std::ofstream stream(outEnv, std::ios::binary);
  expect(stream.good(), "RVC integration output file should open");
  stream.write(reinterpret_cast<const char *>(out.data()),
               static_cast<std::streamsize>(out.size() * sizeof(float)));
  expect(stream.good(), "RVC integration output file should write");
  std::cout << "RVC integration output: " << outEnv << " samples=" << out.size()
            << std::endl;
}

} // namespace

int main() {
  try {
    testSplitPreservesHNSepStateAndUndoRedo();
    testStretchRebuildResamplesHNSepMasterCurves();
    maybeRunRVCIntegrationTest();
    std::cout << "SVCFusionStudio logic tests passed." << std::endl;
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "SVCFusionStudio logic tests failed: " << exception.what()
              << std::endl;
    return 1;
  }
}
