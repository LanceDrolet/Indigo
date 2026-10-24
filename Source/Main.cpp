#include <JuceHeader.h>
#include "MainComponent.h"
#include "Plugins.h"

class IndigoApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    void initialise (const juce::String& commandLine) override
    {
        // Started by the plugin manager to probe plugins: no window, just answer until Indigo hangs up.
        auto worker = std::make_unique<PluginScanWorker>();
        if (worker->initialiseFromCommandLine (commandLine, pluginScanProcessId))
        {
            scanWorker = std::move (worker);
            return;
        }

        window = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override                            { window = nullptr; scanWorker = nullptr; }

    // Offers to save recordings that would be lost first.
    void systemRequestedQuit() override
    {
        if (auto* main = window != nullptr ? dynamic_cast<MainComponent*> (window->getContentComponent()) : nullptr)
            main->confirmDiscardThen ([] { quit(); });
        else
            quit();
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, juce::Colours::black, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (820, 600, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    std::unique_ptr<MainWindow> window;
    std::unique_ptr<PluginScanWorker> scanWorker;
};

START_JUCE_APPLICATION (IndigoApplication)
