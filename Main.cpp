#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <array>
#include <map>
#include <vector>

namespace {
juce::String utf8(const char* value) { return juce::String::fromUTF8(value); }

constexpr std::array<int, 2> rates { 44100, 88200 };
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
        row->setAttribute("category", type->category);
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
    juce::String name, vendor, category, path;
    std::array<int, rates.size()> samples {};
};

}

#include "Export.h"

namespace {
juce::String bundleSignature(const juce::File& bundle)
{
    juce::StringArray metadata;
    const auto record = [&](const juce::File& file)
    {
        metadata.add(file.getRelativePathFrom(bundle) + "\t"
                     + juce::String(file.getSize()) + "\t"
                     + juce::String(file.getLastModificationTime().toMilliseconds()));
    };
    record(bundle);
    for (const auto& entry : juce::RangedDirectoryIterator(
             bundle, true, "*", juce::File::findFiles, juce::File::FollowSymlinks::noCycles))
        record(entry.getFile());
    metadata.sort(false);
    return juce::String::toHexString(metadata.joinIntoString("\n").hashCode64());
}

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
        header.addColumn(utf8("Категория"), 3, 145);
        for (size_t i = 0; i < rates.size(); ++i)
            header.addColumn(juce::String(rates[i] / 1000.0, 1).replaceCharacter('.', ',')
                                 + utf8(" кГц"), static_cast<int>(i) + 4, 126);
        header.setSortColumnId(1, true);
        addAndMakeVisible(table);
        status.setJustificationType(juce::Justification::centredLeft);
        details.setText(utf8("В ячейках указаны миллисекунды и сэмплы. «—» означает, что задержку получить не удалось."),
                        juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, juce::Colours::white);
        details.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(refreshButton);
        refreshButton.onClick = [this] { beginScan(); };
        addAndMakeVisible(status);
        addAndMakeVisible(details);
        addAndMakeVisible(saveText);
        addAndMakeVisible(saveExcel);
        saveText.setEnabled(false);
        saveExcel.setEnabled(false);
        saveText.onClick = [this] { exportResults(false); };
        saveExcel.onClick = [this] { exportResults(true); };
        beginScan();
    }

    ~Results() override
    {
        stopTimer();
        if (worker != nullptr)
        {
            if (worker->isRunning())
                worker->kill();
            else
                recordResult();
        }
        result.deleteFile();
        table.setModel(nullptr);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(16);
        auto toolbar = area.removeFromTop(32);
        refreshButton.setBounds(toolbar.removeFromLeft(102).reduced(3));
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
        if (column <= 3)
            value = column == 1 ? item.name : column == 2 ? item.vendor : item.category;
        else
        {
            const auto index = static_cast<size_t>(column - 4);
            const auto samples = item.samples[index];
            value = samples < 0 ? utf8("—")
                                : juce::String(1000.0 * samples / rates[index], 3)
                                      .replaceCharacter('.', ',')
                                      + utf8(" мс (") + juce::String(samples) + ")";
        }
        graphics.setColour(juce::Colours::white);
        graphics.drawText(value, 8, 0, width - 16, height,
                          column <= 3 ? juce::Justification::centredLeft
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
            else if (sortColumn == 3)
                order = left.category.compareNatural(right.category);
            else if (sortColumn >= 4)
            {
                const auto index = static_cast<size_t>(sortColumn - 4);
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

    void appendRows(const juce::XmlElement& scan, const juce::String& path)
    {
        for (auto* plugin : scan.getChildIterator())
        {
            Row row;
            row.name = plugin->getStringAttribute("name");
            row.vendor = plugin->getStringAttribute("vendor");
            row.category = plugin->getStringAttribute("category");
            row.path = path;
            for (size_t i = 0; i < rates.size(); ++i)
                row.samples[i] = plugin->getIntAttribute("hz" + juce::String(rates[i]), -1);
            rows.push_back(std::move(row));
        }
    }

    void writeCache()
    {
        if (auto directory = cacheFile.getParentDirectory().createDirectory(); directory.failed())
        {
            cacheWriteFailed = true;
            return;
        }
        juce::TemporaryFile temporary(cacheFile);
        cacheWriteFailed = !cache.writeTo(temporary.getFile())
                           || !temporary.overwriteTargetFileWithTemporary();
    }

    void loadCache()
    {
        auto saved = cache.getNumChildElements() > 0
                         ? std::make_unique<juce::XmlElement>(cache)
                         : juce::parseXML(cacheFile);
        cache.deleteAllChildElements();
        if (saved == nullptr || !saved->hasTagName("vst3LatencyCache1"))
            return;

        for (auto* entry : saved->getChildIterator())
        {
            const auto path = entry->getStringAttribute("path");
            const auto signature = signatures.find(path);
            if (signature != signatures.end() && entry->getNumChildElements() > 0
                && entry->getStringAttribute("signature") == signature->second)
            {
                appendRows(*entry, path);
                cache.addChildElement(new juce::XmlElement(*entry));
                paths.removeString(path);
            }
        }
        writeCache();
    }

    void recordResult()
    {
        auto xml = worker->getExitCode() == 0 ? juce::parseXML(result) : nullptr;
        const auto path = paths[next - 1];
        if (xml != nullptr && xml->hasTagName("scan") && xml->getNumChildElements() > 0)
        {
            appendRows(*xml, path);
            const auto signature = signatures.at(path);
            if (bundleSignature(juce::File(path)) == signature)
            {
                xml->setAttribute("path", path);
                xml->setAttribute("signature", signature);
                cache.addChildElement(xml.release());
                writeCache();
            }
        }
        else
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

    void beginScan()
    {
        refreshButton.setEnabled(false);
        saveText.setEnabled(false);
        saveExcel.setEnabled(false);
        status.setText(utf8("Проверка установленных VST3…"), juce::dontSendNotification);
        scanPending = true;
        startTimer(50);
    }

    void timerCallback() override
    {
        if (scanPending)
        {
            juce::VST3PluginFormat format;
            auto installed = format.searchPathsForPlugins(format.getDefaultLocationsToSearch(), true, false);
            installed.removeDuplicates(false);
            std::map<juce::String, juce::String> current;
            for (const auto& path : installed)
                current.emplace(path, bundleSignature(juce::File(path)));
            paths = std::move(installed);
            signatures = std::move(current);
            next = 0;
            rows.clear();
            loadCache();
            refresh();
            scanPending = false;
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
            status.setText(signatures.empty() ? utf8("В стандартных папках VST3 плагины не найдены.")
                                          : utf8("Сканирование завершено · строк: ")
                                                + juce::String(getNumRows())
                                                + (cacheWriteFailed ? utf8(" · кэш не сохранён") : juce::String()),
                           juce::dontSendNotification);
            saveText.setEnabled(!rows.empty());
            saveExcel.setEnabled(!rows.empty());
            stopTimer();
            refreshButton.setEnabled(true);
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
    juce::TextButton refreshButton { utf8("Обновить") };
    juce::TextButton saveText { utf8("Сохранить TXT") };
    juce::TextButton saveExcel { utf8("Сохранить Excel") };
    std::vector<Row> rows;
    juce::StringArray paths;
    std::map<juce::String, juce::String> signatures;
    juce::XmlElement cache { "vst3LatencyCache1" };
    const juce::File cacheFile = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                    .getChildFile("Library/Application Support/VST3 Latency/scan-cache.xml");
    bool cacheWriteFailed = false;
    std::unique_ptr<juce::ChildProcess> worker;
    juce::File result;
    double deadline = 0;
    int next = 0, sortColumn = 1;
    bool sortAscending = true, scanPending = false;
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
        setResizeLimits(830, 280, 2400, 1600);
        centreWithSize(860, 560);
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class App final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "VST3 Latency"; }
    const juce::String getApplicationVersion() override { return "1.0.5"; }

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
        if (args.size() == 2 && args[0] == "--check-export")
        {
            const juce::File directory(args[1]);
            const std::vector<Row> example {
                { "Test & XML", "Example", "Fx|Delay", {}, { 441, 882 } }
            };
            const bool saved = saveResults(directory.getChildFile("results.txt"), example, false)
                               && saveResults(directory.getChildFile("results.xlsx"), example, true);
            setApplicationReturnValue(saved ? 0 : 1);
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
