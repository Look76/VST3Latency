#pragma once

namespace {
juce::String xmlPart(juce::String body)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" + body;
}

bool saveResults(const juce::File& file, const std::vector<Row>& rows, bool excel)
{
    juce::StringArray headers { utf8("Плагин"), utf8("Производитель") };
    for (const auto rate : rates)
    {
        const auto frequency = juce::String(rate / 1000.0, 1).replaceCharacter('.', ',');
        headers.add(frequency + utf8(" кГц, мс"));
        headers.add(frequency + utf8(" кГц, сэмплы"));
    }

    juce::String text;
    juce::XmlElement worksheet("worksheet");
    worksheet.setAttribute("xmlns", "http://schemas.openxmlformats.org/spreadsheetml/2006/main");
    auto* sheet = worksheet.createNewChildElement("sheetData");
    const auto append = [&](const juce::StringArray& values, bool numeric)
    {
        if (!excel)
        {
            juce::StringArray fields;
            for (const auto& value : values)
                fields.add(value.replaceCharacters("\t\r\n", "   "));
            text += fields.joinIntoString("\t") + "\r\n";
            return;
        }

        auto* row = sheet->createNewChildElement("row");
        for (int index = 0; index < values.size(); ++index)
        {
            auto* cell = row->createNewChildElement("c");
            if (numeric && index >= 2)
            {
                if (values[index].isNotEmpty())
                    cell->createNewChildElement("v")->addTextElement(values[index]);
            }
            else
            {
                cell->setAttribute("t", "inlineStr");
                auto* value = cell->createNewChildElement("is")->createNewChildElement("t");
                value->setAttribute("xml:space", "preserve");
                value->addTextElement(values[index]);
            }
        }
    };

    append(headers, false);
    for (const auto& row : rows)
    {
        juce::StringArray values { row.name, row.vendor };
        for (size_t i = 0; i < rates.size(); ++i)
        {
            const int samples = row.samples[i];
            auto milliseconds = samples < 0 ? juce::String()
                                            : juce::String(1000.0 * samples / rates[i], 6);
            values.add(excel ? milliseconds : milliseconds.replaceCharacter('.', ','));
            values.add(samples < 0 ? juce::String() : juce::String(samples));
        }
        append(values, true);
    }

    if (!excel)
        return file.replaceWithText(text);

    juce::ZipFile::Builder archive;
    const auto add = [&](const char* name, const juce::String& contents)
    {
        archive.addEntry(new juce::MemoryInputStream(contents.toRawUTF8(),
                                                    contents.getNumBytesAsUTF8(), true),
                         6, name, juce::Time::getCurrentTime());
    };
    add("[Content_Types].xml", xmlPart(R"xml(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/></Types>)xml"));
    add("_rels/.rels", xmlPart(R"xml(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>)xml"));
    add("xl/workbook.xml", xmlPart(R"xml(<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="VST3 Latency" sheetId="1" r:id="rId1"/></sheets></workbook>)xml"));
    add("xl/_rels/workbook.xml.rels", xmlPart(R"xml(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/></Relationships>)xml"));
    add("xl/worksheets/sheet1.xml", worksheet.toString());

    juce::TemporaryFile temporary(file);
    {
        juce::FileOutputStream output(temporary.getFile());
        if (!output.openedOk() || !archive.writeToStream(output, nullptr))
            return false;
        output.flush();
        if (output.getStatus().failed())
            return false;
    }
    return temporary.overwriteTargetFileWithTemporary();
}
}
