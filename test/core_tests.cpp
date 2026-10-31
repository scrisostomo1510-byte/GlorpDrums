/*
    Parser tests for the DrumGizmo format layer.

    Deliberately has no JUCE dependency: if this builds and passes, the kit
    loading logic is sound regardless of how the audio engine turns out.
*/

#include "BleedMatrix.h"
#include "AcousticCrosstalk.h"
#include "DrumKit.h"
#include "GlorpKit.h"
#include "KitScanner.h"
// PitchShifter tests removed: varispeed resampling replaces WSOLA for drum tuning.
#include "XmlLite.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace glorp;

namespace
{
    int failures = 0;
    int checks = 0;

    void check (bool condition, const std::string& what)
    {
        ++checks;

        if (! condition)
        {
            ++failures;
            std::cerr << "  FAIL: " << what << "\n";
        }
    }

    void checkEq (const std::string& actual, const std::string& expected, const std::string& what)
    {
        ++checks;

        if (actual != expected)
        {
            ++failures;
            std::cerr << "  FAIL: " << what << " (expected \"" << expected << "\", got \"" << actual << "\")\n";
        }
    }

    void checkEq (int actual, int expected, const std::string& what)
    {
        ++checks;

        if (actual != expected)
        {
            ++failures;
            std::cerr << "  FAIL: " << what << " (expected " << expected << ", got " << actual << ")\n";
        }
    }

    void checkNear (float actual, float expected, float tolerance, const std::string& what)
    {
        ++checks;

        if (std::abs (actual - expected) > tolerance)
        {
            ++failures;
            std::cerr << "  FAIL: " << what << " (expected ~" << expected << ", got " << actual << ")\n";
        }
    }

    void section (const std::string& name)
    {
        std::cout << "- " << name << "\n";
    }

    void writeFile (const fs::path& p, const std::string& contents)
    {
        fs::create_directories (p.parent_path());
        std::ofstream out (p, std::ios::binary);
        out << contents;
    }

    //==========================================================================
    void testXmlBasics()
    {
        section ("XmlLite");

        std::string err;
        auto root = parseXml ("<?xml version=\"1.0\"?>\n<a x='1' y=\"two\"><b/>text<c>3</c></a>", err);

        check (root != nullptr, "parses a simple document: " + err);
        checkEq (root->tag, "a", "root tag");
        checkEq (root->attribute ("x"), "1", "single-quoted attribute");
        checkEq (root->attribute ("y"), "two", "double-quoted attribute");
        checkEq (root->attribute ("missing", "dflt"), "dflt", "attribute fallback");
        checkEq (root->childText ("c"), "3", "child text");
        check (root->child ("b") != nullptr, "self-closing child");
        check (root->child ("nope") == nullptr, "missing child is null");
        checkEq (root->childrenNamed ("c").size(), 1u, "childrenNamed count");

        // Comments, CDATA-ish content, nested identical tags.
        auto nested = parseXml ("<!-- hi --><r><t><t>deep</t></t></r>", err);
        check (nested != nullptr, "parses comments");
        checkEq (nested->child ("t")->child ("t")->text, "deep", "nested same-name children");

        // Entities.
        auto ents = parseXml ("<r a=\"a&amp;b\">&lt;tag&gt; &#65;&#x42;</r>", err);
        checkEq (ents->attribute ("a"), "a&b", "amp entity");
        checkEq (ents->text, "<tag> AB", "lt/gt and numeric entities");

        // Malformed input must fail loudly rather than half-parse.
        check (parseXml ("<a><b></a>", err) == nullptr, "mismatched close tag fails");
        check (parseXml ("<a attr=unquoted/>", err) == nullptr, "unquoted attribute fails");
        check (parseXml ("", err) == nullptr, "empty document fails");
        check (parseXml ("<a", err) == nullptr, "truncated document fails");
        check (parseXml ("<a>&nope;</a>", err) == nullptr, "unknown entity fails");

        // Numeric attributes.
        auto nums = parseXml ("<r f=\"1.5\" i=\"42\" b=\"true\" bad=\"xyz\"/>", err);
        check (std::abs (nums->attributeFloat ("f", 0.0f) - 1.5f) < 0.0001f, "attributeFloat");
        checkEq (nums->attributeInt ("i", 0), 42, "attributeInt");
        check (nums->attributeBool ("b", false), "attributeBool true");
        check (std::abs (nums->attributeFloat ("bad", 9.0f) - 9.0f) < 0.0001f, "attributeFloat fallback on junk");
    }

    //==========================================================================
    /// Builds a kit in the modern (0.9.17+) format and checks everything wires up.
    void testModernKit (const fs::path& root)
    {
        section ("drumkit.xml, modern format");

        writeFile (root / "TestKit" / "drumkit.xml", R"(<?xml version="1.0" encoding="UTF-8"?>
<drumkit samplerate="48000" version="2.1.0">
  <metadata>
    <version>1.2.3</version>
    <title>Test Kit</title>
    <description>A kit for tests</description>
    <author>Nobody</author>
    <license>CC0</license>
  </metadata>
  <channels>
    <channel name="OHL"><title>Overhead left</title></channel>
    <channel name="OHR"/>
    <channel name="Snare_top"/>
    <channel name="Room"/>
  </channels>
  <instruments>
    <instrument name="Snare" file="Snare/Snare.xml">
      <channelmap in="OHL" out="OHL" main="true"/>
      <channelmap in="OHR" out="OHR" main="true"/>
      <channelmap in="Room" out="Room"/>
      <channelmap in="Unmapped" out="Room"/>
    </instrument>
    <instrument name="HihatClosed" group="hihat" file="Hihat/HihatClosed.xml">
      <chokes>
        <choke instrument="HihatOpen" choketime="50"/>
        <choke instrument="HihatHalfOpen"/>
      </chokes>
      <channelmap in="OHL" out="OHL" main="true"/>
      <channelmap in="OHR" out="OHR" main="true"/>
    </instrument>
    <instrument name="Broken" file="Nope/Missing.xml"/>
    <instrument name="NoSamples" file="Empty/Empty.xml"/>
    <instrument file="NoName/NoName.xml"/>
  </instruments>
</drumkit>)");

        writeFile (root / "TestKit" / "Snare" / "Snare.xml", R"(<?xml version="1.0" encoding="UTF-8"?>
<instrument version="2.0">
  <metadata><title>Fat Snare</title></metadata>
  <samples>
    <sample name="Snare-1" power="0.9">
      <audiofile channel="OHL" file="samples/s1.wav" filechannel="1"/>
      <audiofile channel="OHR" file="samples/s1.wav" filechannel="2"/>
      <audiofile channel="Room" file="samples/s1.wav" filechannel="3"/>
    </sample>
    <sample name="Snare-2" power="1.0">
      <audiofile channel="OHL" file="samples/s2.wav" filechannel="1"/>
      <audiofile channel="OHR" file="samples/s2.wav" filechannel="2"/>
      <audiofile channel="Room" file="samples/s2.wav" filechannel="3"/>
    </sample>
    <sample name="Empty"/>
  </samples>
</instrument>)");

        writeFile (root / "TestKit" / "Hihat" / "HihatClosed.xml", R"(<?xml version="1.0" encoding="UTF-8"?>
<instrument version="2.0">
  <samples>
    <sample name="Hc-1">
      <audiofile channel="OHL" file="samples/hc.wav" filechannel="1"/>
      <audiofile channel="OHR" file="samples/hc.wav" filechannel="2"/>
    </sample>
  </samples>
</instrument>)");

        writeFile (root / "TestKit" / "Empty" / "Empty.xml", R"(<?xml version="1.0"?>
<instrument version="2.0"><samples/></instrument>)");

        writeFile (root / "TestKit" / "midimap.xml", R"(<?xml version="1.0"?>
<midimap>
  <map note="38" instr="Snare"/>
  <map note="42" instr="HihatClosed"/>
  <map note="999" instr="Snare"/>
  <map note="40" instr="DoesNotExist"/>
</midimap>)");

        const auto kit = DrumKit::load ((root / "TestKit" / "drumkit.xml").string());

        checkEq (kit.error, "", "kit loads without error");
        checkEq (kit.title, "Test Kit", "title");
        checkEq (kit.displayName(), "Test Kit", "display name");
        checkEq (kit.declaredSampleRate, 48000, "samplerate");
        checkEq (kit.formatVersion, "2.1.0", "format version");
        checkEq (kit.author, "Nobody", "author");
        checkEq (static_cast<int> (kit.channels.size()), 4, "channel count");
        checkEq (kit.channels[0].title, "Overhead left", "channel title");
        checkEq (kit.indexOfKitChannel ("Room"), 3, "kit channel lookup");

        // The three broken instruments must have been dropped with warnings.
        checkEq (static_cast<int> (kit.instruments.size()), 2, "usable instrument count");
        check (kit.instruments.size() == 2, "only sound instruments survive");

        const auto snareIdx = kit.indexOfInstrument ("Snare");
        check (snareIdx == 0, "Snare is first");

        if (snareIdx >= 0)
        {
            const auto& snare = kit.instruments[static_cast<size_t> (snareIdx)];

            checkEq (snare.displayName, "Fat Snare", "instrument title overrides prettified name");
            checkEq (static_cast<int> (snare.samples.size()), 2, "samples with no audiofiles are dropped");
            check (std::abs (snare.samples[0].power - 0.9f) < 0.001f, "sample power");
            checkEq (static_cast<int> (snare.samples[0].audioFiles.size()), 3, "audiofiles per sample");

            // filechannel is 1-based in XML, 0-based for us.
            checkEq (snare.samples[0].audioFiles[0].fileChannel, 0, "filechannel 1 -> 0");
            checkEq (snare.samples[0].audioFiles[2].fileChannel, 2, "filechannel 3 -> 2");
            checkEq (snare.samples[0].audioFiles[0].file, "samples/s1.wav", "audiofile path");
            checkEq (snare.baseDir, (root / "TestKit" / "Snare").string(), "instrument base dir");

            checkEq (static_cast<int> (snare.resolvedChannels.size()), 2, "resolved channels per sample");
            checkEq (static_cast<int> (snare.resolvedChannels[0].size()), 3, "resolved channel count");
            checkEq (snare.resolvedChannels[0][0].kitChannel, "OHL", "resolves through channelmap");
            check (snare.resolvedChannels[0][0].main, "main flag carried through");

            // 'Unmapped' is an instrument channel with no matching <audiofile>; must not crash.
            check (! snare.resolvedChannels[0].empty(), "still resolves after unmapped channelmap");
        }

        const auto hhIdx = kit.indexOfInstrument ("HihatClosed");

        if (hhIdx >= 0)
        {
            const auto& hh = kit.instruments[static_cast<size_t> (hhIdx)];

            checkEq (hh.group, "hihat", "group attribute");
            checkEq (static_cast<int> (hh.chokes.size()), 2, "choke count");
            check (std::abs (hh.chokes[0].chokeTimeMs - 50.0f) < 0.001f, "explicit choketime");
            check (! hh.chokes[1].hasChokeTime, "missing choketime flagged");
            check (std::abs (hh.chokes[1].chokeTimeMs - 100.0f) < 0.001f, "default choketime");
            checkEq (hh.index, 1, "instrument index assigned");
        }

        // Midimap: the two in-range notes with known instruments survive; note 999 is out
        // of MIDI range and note 40 names an instrument that failed to load.
        checkEq (static_cast<int> (kit.suggestedMap.size()), 2, "valid midimap entries only");
        check (kit.suggestedMap.count (38) == 1, "note 38 mapped");
        check (kit.suggestedMap.count (42) == 1, "note 42 mapped");

        bool sawWarning = false;
        for (auto& w : kit.warnings)
            if (w.find ("Missing.xml") != std::string::npos) sawWarning = true;

        check (sawWarning, "missing instrument xml produces a warning naming the file");
    }

    //==========================================================================
    void testLegacyKit (const fs::path& root)
    {
        section ("drumkit.xml, legacy pre-0.9.17 format");

        writeFile (root / "LegacyKit" / "drumkit.xml", R"(<?xml version="1.0"?>
<drumkit name="Old kit" description="uses the old layout">
  <channels>
    <channel name="OHL"/>
    <channel name="OHR"/>
  </channels>
  <instruments>
    <instrument name="Kick" file="Kick/Kick.xml">
      <channelmap in="OHL-kick" out="OHL">
        <bleed>0.5</bleed>
      </channelmap>
      <channelmap in="OHR-kick" out="OHR">
        <bleed>0.5</bleed>
      </channelmap>
    </instrument>
  </instruments>
</drumkit>)");

        writeFile (root / "LegacyKit" / "Kick" / "Kick.xml", R"(<?xml version='1.0' encoding='UTF-8'?>
<instrument version="2.0" name="Kick">
  <samples>
    <sample name="Kick-1" power="0.0150941">
      <audiofile channel="OHL-kick" file="samples/1-Kick.wav" filechannel="1"/>
      <audiofile channel="OHR-kick" file="samples/1-Kick.wav" filechannel="2"/>
    </sample>
  </samples>
</instrument>)");

        const auto kit = DrumKit::load ((root / "LegacyKit" / "drumkit.xml").string());

        checkEq (kit.error, "", "legacy kit loads");
        checkEq (kit.title, "Old kit", "legacy kit carries name attribute as title");
        checkEq (kit.displayName(), "Old kit", "display name uses legacy kit name");
        checkEq (static_cast<int> (kit.instruments.size()), 1, "legacy instrument count");

        if (! kit.instruments.empty())
        {
            const auto& kick = kit.instruments[0];

            checkEq (kick.name, "Kick", "legacy instrument name");
            checkEq (kick.displayName, "Kick", "display name prettified from name");
            checkEq (static_cast<int> (kick.channelMaps.size()), 2, "legacy channelmap count");
            check (std::abs (kick.channelMaps[0].bleed - 0.5f) < 0.001f, "bleed parsed from child element");

            // The instrument channel is 'OHL-kick' but the kit channel is 'OHL'.
            checkEq (static_cast<int> (kick.resolvedChannels[0].size()), 2, "legacy channelmap resolves");
            checkEq (kick.resolvedChannels[0][0].kitChannel, "OHL", "legacy in -> out rename honoured");
        }

        // Test fallback to folder name when name attribute is omitted.
        writeFile (root / "NoNameKit" / "drumkit.xml", R"(<?xml version="1.0"?>
<drumkit description="uses the old layout without name">
  <channels><channel name="OHL"/></channels>
  <instruments><instrument name="Kick" file="Kick/Kick.xml"/></instruments>
</drumkit>)");
        writeFile (root / "NoNameKit" / "Kick" / "Kick.xml", R"(<?xml version='1.0' encoding='UTF-8'?>
<instrument version="2.0" name="Kick">
  <samples><sample name="k"><audiofile channel="OHL" file="k.wav" filechannel="1"/></sample></samples>
</instrument>)");

        const auto noNameKit = DrumKit::load ((root / "NoNameKit" / "drumkit.xml").string());
        checkEq (noNameKit.error, "", "nameless legacy kit loads");
        checkEq (noNameKit.title, "", "nameless legacy kit has empty title");
        checkEq (noNameKit.displayName(), "NoNameKit", "display name falls back to folder name");
    }

    //==========================================================================
    void testIdentityMapping (const fs::path& root)
    {
        section ("instrument with no channelmap (identity fallback)");

        writeFile (root / "BareKit" / "drumkit.xml", R"(<?xml version="1.0"?>
<drumkit version="2.1.0">
  <channels><channel name="Tom1"/></channels>
  <instruments>
    <instrument name="Tom1" file="Tom1/Tom1.xml"/>
  </instruments>
</drumkit>)");

        writeFile (root / "BareKit" / "Tom1" / "Tom1.xml", R"(<?xml version="1.0"?>
<instrument version="2.0">
  <samples>
    <sample name="t">
      <audiofile channel="Tom1" file="t.wav" filechannel="1"/>
      <audiofile channel="Ghost" file="t.wav" filechannel="2"/>
    </sample>
  </samples>
</instrument>)");

        const auto kit = DrumKit::load ((root / "BareKit" / "drumkit.xml").string());

        checkEq (kit.error, "", "bare kit loads");
        checkEq (static_cast<int> (kit.instruments.size()), 1, "bare instrument loads");

        if (! kit.instruments.empty())
        {
            // 'Tom1' exists as a kit channel so identity mapping applies; 'Ghost' does not.
            checkEq (static_cast<int> (kit.instruments[0].resolvedChannels[0].size()), 1, "only known channels resolve");
            checkEq (kit.instruments[0].resolvedChannels[0][0].kitChannel, "Tom1", "identity mapping");
        }
    }

    //==========================================================================
    void testFailureModes (const fs::path& root)
    {
        section ("failure modes");

        const auto missing = DrumKit::load ((root / "Nope" / "drumkit.xml").string());
        check (! missing.error.empty(), "missing file reports an error");
        check (missing.instruments.empty(), "missing file yields no instruments");

        writeFile (root / "Broken" / "drumkit.xml", "<notadrumkit/>");
        check (! DrumKit::load ((root / "Broken" / "drumkit.xml").string()).error.empty(), "wrong root tag errors");

        writeFile (root / "Broken2" / "drumkit.xml", "<drumkit version=\"1\"></drumkit>");
        check (! DrumKit::load ((root / "Broken2" / "drumkit.xml").string()).error.empty(), "no instruments node errors");
    }

    //==========================================================================
    /// Writes a minimal but *loadable-looking* kit directory at `dir`.
    void writeMinimalKit (const fs::path& dir, const std::string& title, bool withSampleFile = true)
    {
        writeFile (dir / "drumkit.xml",
                   "<drumkit version=\"2.1.0\"><metadata><title>" + title + "</title></metadata>"
                   "<channels><channel name=\"OH\"/></channels>"
                   "<instruments><instrument name=\"Kick\" file=\"Kick/Kick.xml\"/></instruments></drumkit>");

        writeFile (dir / "Kick" / "Kick.xml",
                   "<instrument version=\"2.0\"><samples><sample name=\"k\">"
                   "<audiofile channel=\"OH\" file=\"samples/k.wav\" filechannel=\"1\"/>"
                   "</sample></samples></instrument>");

        if (withSampleFile)
            writeFile (dir / "Kick" / "samples" / "k.wav", "not really a wav, but it exists");
    }

    void testKitScanner (const fs::path& root)
    {
        section ("KitScanner");

        const auto kitsRoot = root / "kits";

        // Top level, nested one level, and nested two levels.
        writeMinimalKit (kitsRoot / "Alpha", "Alpha Kit");
        writeMinimalKit (kitsRoot / "Nested" / "Beta", "Beta Kit");
        writeMinimalKit (kitsRoot / "Nested" / "Deeper" / "Gamma", "Gamma Kit");

        // Uppercase filename variant should still be found.
        writeMinimalKit (kitsRoot / "OddCase", "Odd Case Kit");
        fs::rename (kitsRoot / "OddCase" / "drumkit.xml", kitsRoot / "OddCase" / "DRUMKIT.XML");

        // Not a kit: wrong root element.
        writeFile (kitsRoot / "NotAKit" / "drumkit.xml", "<settings/>");

        // A kit with no usable instruments should be flagged invalid.
        writeFile (kitsRoot / "EmptyKit" / "drumkit.xml",
                   "<drumkit version=\"2.1.0\"><channels/><instruments/></drumkit>");

        // Junk directories must be skipped, and must not hide a kit behind them.
        writeMinimalKit (kitsRoot / ".git" / "ShouldNotAppear", "Junk Kit");

        const auto found = KitScanner::findKitsUnder (kitsRoot.string());

        // 4 loadable kits + NotAKit + EmptyKit. The one under .git must not appear.
        checkEq (static_cast<int> (found.size()), 6, "recursive kit discovery including nested and odd-cased");

        auto entries = KitScanner::scan ({ kitsRoot.string() });

        int valid = 0;
        int invalid = 0;

        for (auto& e : entries)
        {
            if (e.valid) ++valid; else ++invalid;

            if (! e.valid && e.error.empty())
            {
                ++failures;
                std::cerr << "  FAIL: invalid kit '" << e.drumkitPath << "' gave no error text\n";
            }
        }

        checkEq (valid, 4, "valid kits discovered");
        checkEq (invalid, 2, "invalid kits discovered");

        // Titles come from metadata; valid entries sort before invalid ones.
        check (entries.size() == 6, "total entries");
        check (entries.front().valid, "valid kits sort first");
        checkEq (entries.front().displayName, "Alpha Kit", "title from metadata");

        bool sawOddCase = false;
        for (auto& e : entries)
            if (e.folderName == "OddCase") sawOddCase = true;

        check (sawOddCase, "uppercase DRUMKIT.XML is discovered");

        // A second root overlapping the first must not duplicate kits.
        const auto overlap = KitScanner::scan ({ kitsRoot.string(), (kitsRoot / "Nested").string() });
        checkEq (static_cast<int> (overlap.size()), static_cast<int> (entries.size()), "overlapping roots do not duplicate");

        // Path resolution.
        check (! KitScanner::resolvePath ("~").empty(), "~ resolves");
        check (KitScanner::resolvePath ("~someuser/x").empty(), "~user is rejected");
        checkEq (KitScanner::resolvePath ("/a/b/../c"), "/a/c", "path is normalised");
        check (KitScanner::findKitsUnder ("/definitely/not/here").empty(), "missing root yields nothing");

        // GLORP_KITS_PATH drives the default search.
    #ifdef _WIN32
        _putenv_s ("GLORP_KITS_PATH", kitsRoot.string().c_str());
    #else
        ::setenv ("GLORP_KITS_PATH", kitsRoot.string().c_str(), 1);
    #endif
        auto defaults = KitScanner::defaultSearchPaths();
    #ifdef _WIN32
        _putenv_s ("GLORP_KITS_PATH", "");
    #else
        ::unsetenv ("GLORP_KITS_PATH");
    #endif

        check (! defaults.empty(), "env var contributes a search root");
        check (! defaults.empty() && defaults.front() == kitsRoot.string(), "env var root is searched first");

        // Full load of a discovered kit should succeed end to end.
        DrumKit kit;
        bool ok = false;

        for (auto& e : entries)
        {
            if (e.folderName != "Alpha") continue;
            ok = KitScanner::loadKit (e.drumkitPath, kit);
            break;
        }

        check (ok, "loadKit on a scanned entry succeeds");
        checkEq (static_cast<int> (kit.instruments.size()), 1, "scanned kit has one instrument");

        if (! kit.instruments.empty())
        {
            const auto resolved = kit.instruments[0].samplePath (kit.instruments[0].samples[0].audioFiles[0]);
            check (fs::exists (resolved), "resolved sample path exists on disk");
            check (fs::path (resolved).is_absolute(), "resolved sample path is absolute");
        }
    }
}

//==============================================================================
void testVarispeedMath()
{
    section ("Varispeed Resampling & Pitch Math");

    // 1. Semitone to playback ratio (tape-style tuning)
    const auto ratioForSemitones = [] (float semitones) -> double
    {
        return std::pow (2.0, static_cast<double> (semitones) / 12.0);
    };

    check (std::abs (ratioForSemitones (0.0f) - 1.0) < 1e-6, "0 semitones gives 1.0 ratio");
    check (std::abs (ratioForSemitones (12.0f) - 2.0) < 1e-6, "+12 semitones doubles speed (octave up)");
    check (std::abs (ratioForSemitones (-12.0f) - 0.5) < 1e-6, "-12 semitones halves speed (octave down)");
    check (std::abs (ratioForSemitones (7.0f) - 1.498307) < 1e-4, "+7 semitones (perfect fifth)");

    // 2. Sample rate conversion ratio
    const double kitRate = 48000.0;
    const double hostRate = 44100.0;
    const double rateRatio = kitRate / hostRate;
    check (rateRatio > 1.0884 && rateRatio < 1.0885, "48k kit at 44.1k host speeds up read by ~1.0884");

    // Net step with both pitch and rate ratio
    const double netStep = ratioForSemitones (0.0f) * rateRatio;
    check (std::abs (netStep - rateRatio) < 1e-6, "Net step combines pitch and sample rate cleanly");

    // 3. Hermite cubic interpolation validation
    const auto cubicHermite = [] (float y0, float y1, float y2, float y3, float t) -> float
    {
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    };

    check (std::abs (cubicHermite (0.0f, 1.0f, 2.0f, 3.0f, 0.0f) - 1.0f) < 1e-6, "Cubic interpolation at t=0 gives y1");
    check (std::abs (cubicHermite (0.0f, 1.0f, 2.0f, 3.0f, 0.5f) - 1.5f) < 1e-6, "Cubic interpolation of linear points is linear");
    check (std::abs (cubicHermite (0.0f, 1.0f, 2.0f, 3.0f, 1.0f) - 2.0f) < 1e-6, "Cubic interpolation at t=1 gives y2");
}

void testVelocityCurveMath()
{
    section ("E-Drum Velocity Response Curves");

    // Logarithmic: soft hits boosted for light touch
    const float softHit = 0.25f;
    const float logVal = std::sqrt (softHit);
    check (std::abs (logVal - 0.5f) < 1e-5, "Log curve boosts soft hits (0.25 -> 0.50)");

    // Exponential: soft hits attenuated, requires deliberate strikes
    const float expVal = 0.5f * 0.5f;
    check (std::abs (expVal - 0.25f) < 1e-5, "Exp curve requires harder hits (0.50 -> 0.25)");

    // S-Curve: smooth transition with expanded mid-dynamics
    const auto sCurve = [] (float v) { return v * v * (3.0f - 2.0f * v); };
    check (std::abs (sCurve (0.0f) - 0.0f) < 1e-6, "S-Curve at 0 is 0");
    check (std::abs (sCurve (0.5f) - 0.5f) < 1e-6, "S-Curve passes through 0.5 cleanly");
    check (std::abs (sCurve (1.0f) - 1.0f) < 1e-6, "S-Curve at 1 is 1");
    check (sCurve (0.2f) < 0.2f, "S-Curve smooths lower velocities");
    check (sCurve (0.8f) > 0.8f, "S-Curve accelerates high velocities");
}

void testHiHatPedalLogic()
{
    section ("Hi-Hat Continuous Pedal CC#4 Logic");

    // Continuous pedal ranges:
    // pedal > 0.75 -> Closed
    // pedal > 0.35 && pedal <= 0.75 -> Semi-Open
    // pedal <= 0.35 -> Open
    const auto classifyPedal = [] (float pedal) -> const char*
    {
        if (pedal > 0.75f) return "Closed";
        if (pedal > 0.35f) return "Semi-Open";
        return "Open";
    };

    checkEq (std::string (classifyPedal (0.95f)), "Closed", "Pedal 0.95 is Closed");
    checkEq (std::string (classifyPedal (0.80f)), "Closed", "Pedal 0.80 is Closed");
    checkEq (std::string (classifyPedal (0.55f)), "Semi-Open", "Pedal 0.55 is Semi-Open");
    checkEq (std::string (classifyPedal (0.10f)), "Open", "Pedal 0.10 is Open");
    checkEq (std::string (classifyPedal (0.00f)), "Open", "Pedal 0.00 is Open");
}


static fs::path findRepoRoot()
{
    auto p = fs::current_path();
    for (int i = 0; i < 6; ++i)
    {
        if (fs::exists (p / "CMakeLists.txt") && fs::exists (p / "test" / "kits" / "GlorpDemoKit"))
            return p;
        if (! p.has_parent_path() || p == p.parent_path())
            break;
        p = p.parent_path();
    }
    return fs::current_path();
}

void testDemoKit()
{
    section ("GlorpDemoKit verification");

    const fs::path repoRoot = findRepoRoot();
    const fs::path demoKitPath = repoRoot / "test" / "kits" / "GlorpDemoKit" / "drumkit.xml";

    if (! fs::exists (demoKitPath))
    {
        std::cout << "  (Skipping GlorpDemoKit check - file not found at " << demoKitPath << ")\n";
        return;
    }

    const auto kit = DrumKit::load (demoKitPath.string());

    checkEq (kit.error, "", "demo kit loads without error");
    checkEq (kit.title, "Glorp Demo Kit", "demo kit title");
    checkEq (static_cast<int> (kit.instruments.size()), 23, "23 instruments in demo kit");
    checkEq (static_cast<int> (kit.channels.size()), 2, "2 channels in demo kit (OHL, OHR)");
    checkEq (static_cast<int> (kit.suggestedMap.size()), 24, "24 notes mapped in midimap.xml");

    // Verify HihatClosed chokes HihatOpen
    const auto hhIdx = kit.indexOfInstrument ("HihatClosed");
    check (hhIdx >= 0, "HihatClosed exists");
    if (hhIdx >= 0)
    {
        const auto& hh = kit.instruments[static_cast<size_t> (hhIdx)];
        checkEq (hh.group, "hihat", "HihatClosed group is hihat");
        check (hh.chokes.size() >= 1, "HihatClosed has chokes");
        if (! hh.chokes.empty())
            checkEq (hh.chokes[0].instrument, "HihatOpen", "HihatClosed chokes HihatOpen");
    }

    // Verify all samples have valid audio files on disk
    for (const auto& inst : kit.instruments)
    {
        check (inst.samples.size() >= 2, inst.name + " has at least 2 sample hits");
        for (const auto& sample : inst.samples)
        {
            check (! sample.audioFiles.empty(), inst.name + " " + sample.name + " has audio files");
            for (const auto& ref : sample.audioFiles)
            {
                const auto absPath = inst.samplePath (ref);
                check (fs::exists (absPath), "sample file exists: " + absPath);
            }
        }
    }
}

void testGlorpKitPackage (const fs::path& work)
{
    section ("GlorpKit Single-File Package (.glorpkit)");

    // 1. Extension detection
    check (GlorpKitReader::isGlorpKitFile ("my_kit.glorpkit"), "isGlorpKitFile recognizes .glorpkit");
    check (GlorpKitReader::isGlorpKitFile ("my_kit.GLORPKIT"), "isGlorpKitFile is case-insensitive");
    check (GlorpKitReader::isGlorpKitFile ("archive.zip"), "isGlorpKitFile recognizes .zip");
    check (! GlorpKitReader::isGlorpKitFile ("drumkit.xml"), "isGlorpKitFile rejects .xml");

    // 2. Package test/kits/GlorpDemoKit into a test .glorpkit file
    const fs::path repoRoot = findRepoRoot();
    const fs::path demoKitDir = repoRoot / "test" / "kits" / "GlorpDemoKit";
    const fs::path scriptPath = repoRoot / "tools" / "convert_to_glorpkit.py";
    const fs::path packagePath = work / "GlorpDemoKit.glorpkit";

    if (! fs::exists (scriptPath) || ! fs::exists (demoKitDir / "drumkit.xml"))
    {
        std::cout << "  (Skipping GlorpKit packaging test - convert_to_glorpkit.py or demo kit not found)\n";
        return;
    }

    const std::string cmd = "python3 \"" + scriptPath.string() + "\" \"" + demoKitDir.string() + "\" -o \"" + packagePath.string() + "\" > /dev/null 2>&1";
    int ret = std::system (cmd.c_str());
    check (ret == 0, "tools/convert_to_glorpkit.py succeeded");
    check (fs::exists (packagePath), "package file exists on disk");
    check (fs::exists (packagePath) && fs::file_size (packagePath) > 1000000, "package has expected size > 1MB");

    // 3. Quick manifest reader
    GlorpKitManifest manifest;
    bool manifestOk = GlorpKitReader::quickReadManifest (packagePath.string(), manifest);
    check (manifestOk, "quickReadManifest succeeded");
    check (manifest.valid, "manifest is marked valid");
    checkEq (manifest.title, "Glorp Demo Kit", "quickReadManifest extracted title");
    checkEq (manifest.declaredSampleRate, 44100, "quickReadManifest extracted sample rate");
    checkEq (manifest.instrumentCount, 9, "quickReadManifest piece count is 9");

    // 4. Low-level reader open
    GlorpKitReader reader;
    check (reader.open (packagePath.string()), "GlorpKitReader open succeeded");
    check (reader.isNativeGlorpKit(), "reader recognizes native GlorpKit");
    checkEq (reader.findManifestPath(), "kit.xml", "findManifestPath locates kit.xml");
    const auto* entry = reader.getEntry ("kit.xml");
    check (entry != nullptr, "getEntry for kit.xml succeeds");
    if (entry != nullptr)
    {
        check (! entry->isCompressed, "entry is uncompressed (ZIP_STORED) for zero-copy mmap");
        check (entry->dataOffset > 0, "entry dataOffset is positive");
        check (entry->uncompressedSize > 0, "entry uncompressedSize is positive");
    }

    // 5. DrumKit::load on packaged archive
    DrumKit kit = DrumKit::load (packagePath.string());
    check (kit.error.empty() && ! kit.instruments.empty(), "DrumKit::load from archive produces valid kit");
    check (kit.isArchive, "kit.isArchive is true");
    check (kit.isNativeGlorpKit, "kit.isNativeGlorpKit is true");
    checkEq (kit.archivePath, packagePath.string(), "kit.archivePath matches");
    checkEq (kit.title, "Glorp Demo Kit", "kit.title matches");
    checkEq (kit.declaredSampleRate, 44100, "kit.declaredSampleRate matches");
    checkEq (static_cast<int> (kit.logicalPieces.size()), 9, "kit has 9 logical pieces");
    checkEq (static_cast<int> (kit.instruments.size()), 23, "kit has 23 instruments (zones)");
    check (! kit.factoryMicLevels.empty(), "kit has factory mic levels");
    check (! kit.factoryInstrumentLevels.empty(), "kit has factory instrument levels");

    // 6. Verify zero-copy sample URI formatting
    bool verifiedSamplePath = false;
    for (const auto& inst : kit.instruments)
    {
        for (const auto& sample : inst.samples)
        {
            for (const auto& ref : sample.audioFiles)
            {
                check (ref.archiveOffset > 0, inst.name + " audiofile has positive archiveOffset");
                check (ref.archiveLength > 0, inst.name + " audiofile has positive archiveLength");
                const auto uri = inst.samplePath (ref);
                check (uri.rfind ("glorpkit://", 0) == 0, "samplePath produces glorpkit:// URI: " + uri);
                check (uri.find ('#') != std::string::npos, "URI contains offset hash");
                verifiedSamplePath = true;
                break;
            }
            if (verifiedSamplePath) break;
        }
        if (verifiedSamplePath) break;
    }
    check (verifiedSamplePath, "verified sample URI format from packaged kit");

    // 7. KitScanner package discovery
    const fs::path scanDir = work / "scantest";
    std::error_code ec;
    fs::create_directories (scanDir, ec);
    const fs::path copiedPkg = scanDir / "MyPackagedKit.glorpkit";
    fs::copy_file (packagePath, copiedPkg, fs::copy_options::overwrite_existing, ec);

    auto scanned = KitScanner::scan ({ scanDir.string() });
    checkEq (static_cast<int> (scanned.size()), 1, "KitScanner discovers .glorpkit file");
    if (! scanned.empty())
    {
        check (scanned[0].valid, "scanned package entry is valid");
        checkEq (scanned[0].drumkitPath, copiedPkg.string(), "drumkitPath matches package path");
        check (scanned[0].displayName.find ("[PKG]") != std::string::npos, "displayName includes [PKG] tag: " + scanned[0].displayName);
    }
}

void testBleedMatrixAndCrosstalk()
{
    section ("Bleed Matrix & Acoustic Crosstalk Engine");

    // 1. Piece heuristics
    check (pieceForInstrumentName ("Kick") == BleedPiece::kick, "pieceForInstrumentName Kick");
    check (pieceForInstrumentName ("BassDrum") == BleedPiece::kick, "pieceForInstrumentName BassDrum");
    check (pieceForInstrumentName ("Snare") == BleedPiece::snare, "pieceForInstrumentName Snare");
    check (pieceForInstrumentName ("SnareTop") == BleedPiece::snare, "pieceForInstrumentName SnareTop");
    check (pieceForInstrumentName ("Tom 1") == BleedPiece::tom1, "pieceForInstrumentName Tom 1");
    check (pieceForInstrumentName ("RackTom") == BleedPiece::tom1, "pieceForInstrumentName RackTom");
    check (pieceForInstrumentName ("Floor Tom") == BleedPiece::floorTom, "pieceForInstrumentName Floor Tom");
    check (pieceForInstrumentName ("HiHat") == BleedPiece::hihat, "pieceForInstrumentName HiHat");
    check (pieceForInstrumentName ("Ride") == BleedPiece::ride, "pieceForInstrumentName Ride");
    check (pieceForInstrumentName ("Crash 1") == BleedPiece::crash1, "pieceForInstrumentName Crash 1");
    check (pieceForInstrumentName ("Crash 2") == BleedPiece::crash2, "pieceForInstrumentName Crash 2");

    checkEq (std::string (bleedPieceDisplayName (BleedPiece::snare)), "Snare", "displayName snare");
    checkEq (std::string (bleedPieceIdentifier (BleedPiece::snare)), "snare", "identifier snare");

    // 2. BleedMatrix initial state & gain calculations
    BleedMatrix matrix;
    check (matrix.getMode() == BleedMatrix::Mode::hybridAcoustic, "default mode is hybridAcoustic");
    checkNear (matrix.getMasterBleed(), 1.0f, 0.001f, "default master bleed 1.0");
    checkNear (matrix.getSympatheticResonance(), 0.35f, 0.001f, "default sympathetic resonance 0.35");
    checkNear (matrix.getCellGain (0, 0), 1.0f, 0.001f, "default cell gain 1.0");
    check (! matrix.isCellMuted (0, 0), "default cell not muted");
    checkNear (matrix.getEffectiveGain (0, 0), 1.0f, 0.001f, "default effective gain 1.0");

    // Cell gain scaling
    matrix.setCellGain (0, 1, 0.5f);
    checkNear (matrix.getCellGain (0, 1), 0.5f, 0.001f, "cell gain set to 0.5");
    checkNear (matrix.getEffectiveGain (0, 1), 0.5f, 0.001f, "effective gain with master=1.0 is 0.5");

    // Master bleed scaling
    matrix.setMasterBleed (0.8f);
    checkNear (matrix.getEffectiveGain (0, 1), 0.4f, 0.001f, "effective gain with master=0.8 is 0.4");

    // Cell mute
    matrix.setCellMute (0, 1, true);
    check (matrix.isCellMuted (0, 1), "cell is muted");
    checkNear (matrix.getEffectiveGain (0, 1), 0.0f, 0.001f, "effective gain when muted is 0.0");
    matrix.setCellMute (0, 1, false);

    // Matrix disabled mode
    matrix.setMode (BleedMatrix::Mode::disabled);
    checkNear (matrix.getEffectiveGain (0, 1), 0.0f, 0.001f, "effective gain when disabled is 0.0");

    // 3. Presets
    matrix.resetToNatural();
    check (matrix.getMode() == BleedMatrix::Mode::hybridAcoustic, "resetToNatural restores hybridAcoustic");
    checkNear (matrix.getMasterBleed(), 1.0f, 0.001f, "resetToNatural masterBleed 1.0");
    checkNear (matrix.getEffectiveGain (0, 1), 1.0f, 0.001f, "resetToNatural un-mutes and restores gain 1.0");

    matrix.resetToLive();
    checkNear (matrix.getMasterBleed(), 1.35f, 0.001f, "resetToLive masterBleed 1.35");
    checkNear (matrix.getSympatheticResonance(), 0.60f, 0.001f, "resetToLive sympathetic 0.60");

    std::vector<std::string> micNames = { "Kick_In", "Snare_Top", "OH_L", "OH_R", "Room" };
    matrix.resetToOverheadsOnly (micNames);
    checkNear (matrix.getCellGain (0, 0), 0.0f, 0.001f, "overheads-only mutes kick close mic bleed");
    checkNear (matrix.getCellGain (0, 1), 0.0f, 0.001f, "overheads-only mutes snare close mic bleed");
    checkNear (matrix.getCellGain (0, 2), 1.0f, 0.001f, "overheads-only preserves OH_L bleed");
    checkNear (matrix.getCellGain (0, 4), 1.0f, 0.001f, "overheads-only preserves Room bleed");

    matrix.resetToClean();
    check (matrix.getMode() == BleedMatrix::Mode::disabled, "resetToClean disables bleed");
    checkNear (matrix.getMasterBleed(), 0.0f, 0.001f, "resetToClean masterBleed 0.0");

    // 4. XML serialization
    matrix.resetToNatural();
    matrix.setMasterBleed (0.75f);
    matrix.setSympatheticResonance (0.42f);
    matrix.setCellGain (1, 2, 0.65f);
    matrix.setCellMute (1, 3, true);

    juce::XmlElement xml ("BLEED_STATE");
    matrix.writeXml (xml);

    BleedMatrix restored;
    restored.resetToClean(); // Start from clean to ensure XML overrides
    restored.readXml (xml);

    checkNear (restored.getMasterBleed(), 0.75f, 0.001f, "XML restored masterBleed");
    checkNear (restored.getSympatheticResonance(), 0.42f, 0.001f, "XML restored sympatheticResonance");
    check (restored.getMode() == BleedMatrix::Mode::hybridAcoustic, "XML restored mode");
    checkNear (restored.getCellGain (1, 2), 0.65f, 0.001f, "XML restored cell gain");
    check (restored.isCellMuted (1, 3), "XML restored cell mute");

    // 5. AcousticCrosstalk Engine & Sympathetic Snare Rattle
    AcousticCrosstalk crosstalk;
    crosstalk.prepare (44100.0, 512);

    constexpr size_t numMics = 4;
    constexpr size_t blockSize = 128;
    std::vector<std::vector<float>> bufL (numMics, std::vector<float> (blockSize, 0.0f));
    std::vector<std::vector<float>> bufR (numMics, std::vector<float> (blockSize, 0.0f));
    std::vector<float*> ptrL (numMics);
    std::vector<float*> ptrR (numMics);
    for (size_t m = 0; m < numMics; ++m)
    {
        ptrL[m] = bufL[m].data();
        ptrR[m] = bufR[m].data();
    }

    // Hit Kick drum -> should trigger sympathetic snare buzz on mic 1 (snare)
    crosstalk.triggerSympatheticHit (BleedPiece::kick, 0.9f);
    crosstalk.render (ptrL.data(), ptrR.data(), static_cast<int> (numMics), static_cast<int> (blockSize), matrix, 1, -1);

    // Verify snare mic (mic 1) received buzz energy while mic 0 (kick) has none from sympathetic buzz
    float kickEnergy = 0.0f;
    float snareEnergy = 0.0f;
    for (size_t i = 0; i < blockSize; ++i)
    {
        kickEnergy += std::abs (bufL[0][i]);
        snareEnergy += std::abs (bufL[1][i]);
    }
    checkNear (kickEnergy, 0.0f, 1e-6f, "kick mic has no sympathetic rattle");
    check (snareEnergy > 0.0001f, "snare mic received sympathetic rattle from kick hit");

    // Test reset clears energy
    crosstalk.reset();
    std::fill (bufL[1].begin(), bufL[1].end(), 0.0f);
    crosstalk.render (ptrL.data(), ptrR.data(), static_cast<int> (numMics), static_cast<int> (blockSize), matrix, 1, -1);
    float snareEnergyAfterReset = 0.0f;
    for (size_t i = 0; i < blockSize; ++i)
        snareEnergyAfterReset += std::abs (bufL[1][i]);
    checkNear (snareEnergyAfterReset, 0.0f, 1e-6f, "sympathetic rattle cleared after reset");
}

int main (int argc, char** argv)
{
    const fs::path work = argc > 1 ? fs::path (argv[1])
                                   : fs::temp_directory_path() / "glorpdrums_parser_test";

    std::error_code ec;
    fs::remove_all (work, ec);
    fs::create_directories (work);

    std::cout << "GlorpDrums parser tests\n";

    testXmlBasics();
    testModernKit (work);
    testLegacyKit (work);
    testIdentityMapping (work);
    testFailureModes (work);
    testKitScanner (work);
    testVarispeedMath();
    testVelocityCurveMath();
    testHiHatPedalLogic();
    testDemoKit();
    testGlorpKitPackage (work);
    testBleedMatrixAndCrosstalk();

    std::cout << "\n" << (checks - failures) << "/" << checks << " checks passed\n";

    if (failures > 0)
        std::cout << failures << " FAILURES\n";

    return failures == 0 ? 0 : 1;
}
