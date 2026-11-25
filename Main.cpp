#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <array>
#include <vector>

namespace {
juce::String utf8(const char* value) { return juce::String::fromUTF8(value); }

constexpr std::array<int, 4> rates { 44100, 48000, 88200, 96000 };
constexpr int blockSize = 512;
constexpr auto workerFlag = "--scan-bundle";

int scanBundle(const juce::String& path, const juce::File& result)
{
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile(types, path);
    juce::XmlElement output("scan");
    for (const auto* type : types)
    {
        auto* row = output.createNewChildElement("plugin");
        row->setAttribute("name", type->name);
        row->setAttribute("vendor", type->manufacturerName);
        for (const auto rate : rates)
        {
            juce::String error;
            auto instance = static_cast<juce::AudioPluginFormat&>(format)
                                .createInstanceFromDescription(*type, rate, blockSize, error);
            if (instance == nullptr)
                continue;

            instance->prepareToPlay(rate, blockSize);
            row->setAttribute("hz" + juce::String(rate), instance->getLatencySamples());
            instance->releaseResources();
        }
    }
    return output.writeTo(result) ? 0 : 1;
}

struct Row
{
    juce::String name, vendor, path;
    std::array<int, rates.size()> samples {};
};

}

#include "Export.h"

namespace {
class Results final : public juce::Component,
                      private juce::TableListBoxModel,
                      private juce::Timer
{
public:
    Results()
    {
        setColour(juce::ResizableWindow::backgroundColourId, juce::Colour(0xff171b22));
        table.setModel(this);
        table.setRowHeight(32);
        auto& header = table.getHeader();
        header.addColumn(utf8("Плагин"), 1, 235);
        header.addColumn(utf8("Производитель"), 2, 155);
        for (size_t i = 0; i < rates.size(); ++i)
            header.addColumn(juce::String(rates[i] / 1000.0, 1).replaceCharacter('.', ',')
                                 + utf8(" кГц"), static_cast<int>(i) + 3, 126);
        header.setSortColumnId(1, true);
        addAndMakeVisible(table);
        status.setJustificationType(juce::Justification::centredLeft);
        details.setText(utf8("В ячейках указаны миллисекунды и сэмплы. «—» означает, что задержку получить не удалось."),
                        juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, juce::Colours::white);
        details.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(status);
        addAndMakeVisible(details);
        addAndMakeVisible(saveText);
        addAndMakeVisible(saveExcel);
        saveText.setEnabled(false);
        saveExcel.setEnabled(false);
        saveText.onClick = [this] { exportResults(false); };
        saveExcel.onClick = [this] { exportResults(true); };
        startTimer(50);
    }

    ~Results() override
    {
        stopTimer();
        if (worker != nullptr)
            worker->kill();
        result.deleteFile();
        table.setModel(nullptr);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(16);
        auto toolbar = area.removeFromTop(32);
        saveExcel.setBounds(toolbar.removeFromRight(108).reduced(3));
        saveText.setBounds(toolbar.removeFromRight(108).reduced(3));
        status.setBounds(toolbar);
        details.setBounds(area.removeFromBottom(30));
        table.setBounds(area);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(findColour(juce::ResizableWindow::backgroundColourId));
    }

private:
    int getNumRows() override { return static_cast<int>(rows.size()); }

    void paintRowBackground(juce::Graphics& graphics, int row, int, int, bool selected) override
    {
        graphics.fillAll(juce::Colour(selected ? 0xff284664
                                               : row % 2 == 0 ? 0xff1e2530 : 0xff242d39));
    }

    void paintCell(juce::Graphics& graphics, int row, int column,
                   int width, int height, bool) override
    {
        if (!juce::isPositiveAndBelow(row, getNumRows()))
            return;

        const auto& item = rows[static_cast<size_t>(row)];
        juce::String value;
        if (column <= 2)
            value = column == 1 ? item.name : item.vendor;
        else
        {
            const auto index = static_cast<size_t>(column - 3);
            const auto samples = item.samples[index];
            value = samples < 0 ? utf8("—")
                                : juce::String(1000.0 * samples / rates[index], 3)
                                      .replaceCharacter('.', ',')
                                      + utf8(" мс (") + juce::String(samples) + ")";
        }
        graphics.setColour(juce::Colours::white);
        graphics.drawText(value, 8, 0, width - 16, height,
                          column <= 2 ? juce::Justification::centredLeft
                                      : juce::Justification::centredRight, true);
    }

    juce::String getCellTooltip(int row, int) override
    {
        return juce::isPositiveAndBelow(row, getNumRows())
                   ? rows[static_cast<size_t>(row)].path : juce::String();
    }

    void sortOrderChanged(int column, bool ascending) override
    {
        sortColumn = column;
        sortAscending = ascending;
        refresh();
    }

    void exportResults(bool excel)
    {
        if (chooser != nullptr)
            return;

        const auto extension = excel ? ".xlsx" : ".txt";
        const auto suggested = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                   .getChildFile(juce::String("VST3 Latency") + extension);
        chooser = std::make_unique<juce::FileChooser>(utf8("Сохранить результаты"), suggested,
                                                       juce::String("*") + extension);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                             [this, excel](const juce::FileChooser& dialog)
        {
            const auto destination = dialog.getResult();
            if (destination != juce::File() && !saveResults(destination, rows, excel))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                    utf8("Ошибка экспорта"), utf8("Не удалось сохранить результаты."), {}, this);
            chooser.reset();
        });
    }

    void refresh()
    {
        std::stable_sort(rows.begin(), rows.end(), [this](const Row& left, const Row& right)
        {
            int order = 0;
            if (sortColumn == 1)
                order = left.name.compareNatural(right.name);
            else if (sortColumn == 2)
                order = left.vendor.compareNatural(right.vendor);
            else if (sortColumn >= 3)
            {
                const auto index = static_cast<size_t>(sortColumn - 3);
                const int a = left.samples[index], b = right.samples[index];
                if ((a < 0) != (b < 0))
                    return b < 0;
                order = (a > b) - (a < b);
            }
            return sortAscending ? order < 0 : order > 0;
        });
        table.updateContent();
        table.repaint();
    }

    void recordResult()
    {
        const auto xml = worker->getExitCode() == 0 ? juce::parseXML(result) : nullptr;
        bool any = false;
        if (xml != nullptr && xml->hasTagName("scan"))
        {
            for (auto* plugin : xml->getChildIterator())
            {
                Row row;
                row.name = plugin->getStringAttribute("name");
                row.vendor = plugin->getStringAttribute("vendor");
                row.path = paths[next - 1];
                for (size_t i = 0; i < rates.size(); ++i)
                    row.samples[i] = plugin->getIntAttribute("hz" + juce::String(rates[i]), -1);
                rows.push_back(std::move(row));
                any = true;
            }
        }
        if (!any)
            recordFailure();
        result.deleteFile();
        refresh();
    }

    void recordFailure()
    {
        Row row;
        row.path = paths[next - 1];
        row.name = juce::File(row.path).getFileNameWithoutExtension();
        row.vendor = utf8("Ошибка загрузки");
        row.samples.fill(-1);
        rows.push_back(std::move(row));
    }

    void timerCallback() override
    {
        if (!enumerated)
        {
            juce::VST3PluginFormat format;
            paths = format.searchPathsForPlugins(format.getDefaultLocationsToSearch(), true, false);
            paths.removeDuplicates(false);
            enumerated = true;
        }
        if (worker != nullptr)
        {
            if (worker->isRunning())
            {
                if (juce::Time::getMillisecondCounterHiRes() < deadline)
                    return;
                worker->kill();
                recordFailure();
                result.deleteFile();
                refresh();
            }
            else
                recordResult();
            worker.reset();
        }
        if (next >= paths.size())
        {
            status.setText(paths.isEmpty() ? utf8("В стандартных папках VST3 плагины не найдены.")
                                          : utf8("Сканирование завершено · строк: ")
                                                + juce::String(getNumRows()),
                           juce::dontSendNotification);
            saveText.setEnabled(!rows.empty());
            saveExcel.setEnabled(!rows.empty());
            stopTimer();
            return;
        }

        const auto path = paths[next++];
        status.setText(utf8("Сканирование ") + juce::String(next) + utf8(" из ")
                           + juce::String(paths.size()) + utf8(" · ") + juce::File(path).getFileName(),
                       juce::dontSendNotification);
        result = juce::File::createTempFile(".xml");
        worker = std::make_unique<juce::ChildProcess>();
        const juce::StringArray command {
            juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
            workerFlag, path, result.getFullPathName()
        };
        if (!worker->start(command, 0))
        {
            worker.reset();
            result.deleteFile();
            recordFailure();
            refresh();
        }
        deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
    }

    juce::TableListBox table;
    juce::Label status, details;
    juce::TextButton saveText { utf8("Сохранить TXT") };
    juce::TextButton saveExcel { utf8("Сохранить Excel") };
    std::vector<Row> rows;
    juce::StringArray paths;
    std::unique_ptr<juce::ChildProcess> worker;
    juce::File result;
    double deadline = 0;
    int next = 0, sortColumn = 1;
    bool sortAscending = true, enumerated = false;
    std::unique_ptr<juce::FileChooser> chooser;
};

class Window final : public juce::DocumentWindow
{
public:
    Window() : DocumentWindow("VST3 Latency", juce::Colour(0xff171b22), allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new Results, false);
        setResizable(true, false);
        setResizeLimits(930, 280, 2400, 1600);
        centreWithSize(960, 560);
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class App final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "VST3 Latency"; }
    const juce::String getApplicationVersion() override { return "1.0.2"; }

    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override
    {
        const auto args = getCommandLineParameterArray();
        if (args.size() == 3 && args[0] == workerFlag)
        {
            setApplicationReturnValue(scanBundle(args[1], juce::File(args[2])));
            quit();
            return;
        }
        window = std::make_unique<Window>();
    }

    void shutdown() override { window.reset(); }

private:
    std::unique_ptr<Window> window;
};
}

START_JUCE_APPLICATION(App)
