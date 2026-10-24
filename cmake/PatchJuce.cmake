# Run in the JUCE source folder after download (FetchContent PATCH_COMMAND). Safe to run more than once.
#
# Turn off LV2 "dynamic manifests". With them on, building the LV2 plugin list loads the library of every
# dyn-manifest bundle into the host process; the naspro bridges then load every LADSPA/DSSI plugin on the
# system, and one bad one (e.g. zynaddsubfx's DSSI build) crashes Indigo at startup, outside the
# out-of-process scanner's protection. Dyn-manifest is deprecated and used almost only by those bridges.
set(config "modules/juce_audio_processors/format_types/LV2_SDK/juce_lv2_config.h")
file(READ "${config}" text)
string(REPLACE "#define LILV_DYN_MANIFEST\n" "// #define LILV_DYN_MANIFEST   (disabled by Indigo, see cmake/PatchJuce.cmake)\n" text "${text}")
file(WRITE "${config}" "${text}")

# LV2 plugin classes: lilv works out the class hierarchy (InstrumentPlugin is a GeneratorPlugin, ...) only
# inside lilv_world_load_all, but JUCE loads its built-in copy of the LV2 spec after that call. On systems
# without the spec installed (no lv2-dev package) every LV2 plugin then has class "Plugin" and none is
# marked as an instrument. Load the spec first.
set(lv2 "modules/juce_audio_processors/format_types/juce_LV2PluginFormat.cpp")
file(READ "${lv2}" text)
string(REPLACE
"    Pimpl()
    {
        loadAllPluginsFromPaths (getDefaultLocationsToSearch());

        const auto tempFile = lv2ResourceFolder.getFile();"
"    Pimpl()
    {
        const auto tempFile = lv2ResourceFolder.getFile();"
    text "${text}")
string(REPLACE
"                world->loadBundle (world->newFileUri (nullptr, pathString.toRawUTF8()));
            }
        }
    }"
"                world->loadBundle (world->newFileUri (nullptr, pathString.toRawUTF8()));
            }
        }

        loadAllPluginsFromPaths (getDefaultLocationsToSearch());   // after the spec (Indigo patch, see PatchJuce.cmake)
    }"
    text "${text}")
file(WRITE "${lv2}" "${text}")

# File tree order: folders first, then files (JUCE only does that on Windows; on Linux it sorts everything
# by name together). The order within each group stays the platform's.
set(tree "modules/juce_gui_basics/filebrowser/juce_FileTreeComponent.cpp")
file(READ "${tree}" text)
string(REPLACE
"                static const OSDependentFileComparisonRules comparisonRules { SystemStats::getOperatingSystemType() };

                return comparisonRules.compare"
"                static const OSDependentFileComparisonRules comparisonRules { SystemStats::getOperatingSystemType() };

                // Folders first (Indigo patch, see PatchJuce.cmake)
                if (const auto dir1 = item1->file.isDirectory(), dir2 = item2->file.isDirectory(); dir1 != dir2)
                    return dir1 ? -1 : 1;

                return comparisonRules.compare"
    text "${text}")
file(WRITE "${tree}" "${text}")

# JACK outputs: let the app name its own output ports. Out of the box JUCE registers one out_N port per channel
# of the chosen device and passes the callback only the ports that are connected, so a channel's number moves
# whenever something is patched or unpatched. When Indigo fills juce::jackOutputPortNames before opening the
# device, it gets exactly those ports, always active (main, metronome, preview stay on fixed channels and show up
# by name in a patchbay), and the selected output channels say which device ports the first ports connect to.
set(jack "modules/juce_audio_devices/native/juce_JackAudio_linux.cpp")
file(READ "${jack}" text)
string(FIND "${text}" "jackOutputPortNames" patched)
if(patched EQUAL -1)
    string(REPLACE
"//==============================================================================
class JackAudioIODevice final : public AudioIODevice
{"
"//==============================================================================
// Indigo patch (see PatchJuce.cmake): set before the device is created to register these output ports instead.
StringArray jackOutputPortNames;

class JackAudioIODevice final : public AudioIODevice
{"
        text "${text}")
    string(REPLACE
"            const StringArray outputChannels (getOutputChannelNames());
            for (int i = 0; i < outputChannels.size(); ++i)
            {
                String outputChannelName;
                outputChannelName << \"out_\" << ++totalNumberOfOutputChannels;
"
"            const bool named = ! jackOutputPortNames.isEmpty() && outputName.isNotEmpty();   // Indigo patch
            const StringArray outputChannels (named ? jackOutputPortNames : getOutputChannelNames());
            for (int i = 0; i < outputChannels.size(); ++i)
            {
                String outputChannelName;
                outputChannelName << \"out_\" << ++totalNumberOfOutputChannels;

                if (named)
                {
                    outputChannelName = outputChannels[i];
                    namedOutputs = true;
                }
"
        text "${text}")
    string(REPLACE
"                if (! outputChannels[index])
                    return;

                jassert (index < outputPorts.size());

                const auto* outputPort = outputPorts[index];"
"                if (! outputChannels[index])
                    return;

                // Indigo patch: named ports connect in order, the first to the first selected device channel
                auto port = index;

                if (namedOutputs)
                {
                    port = 0;
                    for (int b = 0; b < index; ++b)
                        port += outputChannels[b] ? 1 : 0;

                    if (port >= outputPorts.size())
                        return;
                }

                jassert (port < outputPorts.size());

                const auto* outputPort = outputPorts[port];"
        text "${text}")
    string(REPLACE
"        lastError.clear();
        close();
"
"        lastError.clear();
        close();
        selectedOutputChannels = outputChannels;   // Indigo patch
"
        text "${text}")
    string(REPLACE
"    BigInteger getActiveOutputChannels() const override  { return activeOutputChannels; }"
"    BigInteger getActiveOutputChannels() const override  { return namedOutputs ? selectedOutputChannels : activeOutputChannels; }"
        text "${text}")
    string(REPLACE
"            if (juce::jack_port_connected (outputPorts.getUnchecked (i)))
                newOutputChannels.setBit (i);"
"            if (namedOutputs || juce::jack_port_connected (outputPorts.getUnchecked (i)))   // Indigo patch
                newOutputChannels.setBit (i);"
        text "${text}")
    string(REPLACE
"    BigInteger activeInputChannels, activeOutputChannels;
"
"    BigInteger activeInputChannels, activeOutputChannels;
    BigInteger selectedOutputChannels;   // Indigo patch: the device channels asked for in open()
    bool namedOutputs = false;           // Indigo patch: the ports came from jackOutputPortNames
"
        text "${text}")
    file(WRITE "${jack}" "${text}")
endif()

# VST3 restartComponent from a non-message thread: JUCE's ComponentRestarter already defers it to the message thread,
# but the assertion stops a debug build. yabridge-bridged (Windows) plugins call it from their own thread.
set(vst3 "modules/juce_audio_processors/format_types/juce_VST3PluginFormat.cpp")
file(READ "${vst3}" text)
string(REPLACE
"    // report against the plugin.
    JUCE_ASSERT_MESSAGE_THREAD

    componentRestarter.restart (flags);"
"    // report against the plugin.
    // JUCE_ASSERT_MESSAGE_THREAD   (disabled by Indigo, see cmake/PatchJuce.cmake: yabridge plugins do this)

    componentRestarter.restart (flags);"
    text "${text}")
file(WRITE "${vst3}" "${text}")
