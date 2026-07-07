#pragma once

#include <cstddef>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace CuAsm {

/**
 * @brief Tool class for generating a set of text files from a given template.
 *
 *        The file should contain lines with inserting markers defined, such as:
 *
 *            blabla...
 *            \@FT_MARKER.POS1             // define marker POS1
 *
 *            blabla...
 *            \@FT_MARKER.POS2             // define marker POS2
 *
 *            // \@FT_MARKER.IGNORED       // ignored
 *            xx \@FT_MARKER.IGNORED2      // also ignored
 *            ...
 *
 *        In which "POS1", "POS2" are user defined markers.
 *
 *        Then calling setMarker(marker, string) will replace those lines with corresponding texts.
 *        Unset markers will be kept as is, with original marker defining line commented.
 *
 *        NOTE: marker defining line should begin with "\@FT_MARKER." with optional leading spaces or tabs,
 *              otherwise it will be ignored by the parser.
 */
class FileTemplate {
public:
    /**
     * @brief Constructs the template from a template file, splitting it into literal text parts
     *        and marker-defining lines.
     * @param templateName Path to the template file.
     * @param commentString String used to comment out marker-defining lines in the generated output.
     */
    explicit FileTemplate(const std::string& templateName, const std::string& commentString = "//")
        : m_TemplateName(templateName), m_CommentString(commentString) {
        static const std::regex markerPattern(R"(^\s*@FT_MARKER\.(\w+))");

        // Opened in binary mode so line endings are split by hand below, matching python's
        // universal-newline text-mode iteration exactly instead of relying on whatever
        // (possibly absent) CRLF translation the platform's iostream implementation does.
        std::ifstream fin(templateName, std::ios::binary);
        if (!fin) {
            throw std::runtime_error("Cannot open template file \"" + templateName + "\"!");
        }
        std::ostringstream contentStream;
        contentStream << fin.rdbuf();
        const std::string content = contentStream.str();

        std::string buf;
        int iline = 0;
        std::size_t pos = 0;
        while (pos < content.size()) {
            const std::size_t nlPos = content.find_first_of("\r\n", pos);

            std::string line;
            bool hadNewline;
            std::size_t nextPos;
            if (nlPos == std::string::npos) {
                line = content.substr(pos);
                hadNewline = false;
                nextPos = content.size();
            } else {
                line = content.substr(pos, nlPos - pos);
                hadNewline = true;
                nextPos = (content[nlPos] == '\r' && nlPos + 1 < content.size() && content[nlPos + 1] == '\n') ? nlPos + 2 : nlPos + 1;
            }
            pos = nextPos;
            ++iline;

            // Every recognized line ending (\r\n, \r, or \n) collapses to a single '\n', and the
            // final line keeps no trailing newline if the file itself didn't end with one --
            // mirroring python's text-mode iteration precisely.
            const std::string normalizedLine = hadNewline ? line + '\n' : line;

            std::smatch res;
            if (std::regex_search(normalizedLine, res, markerPattern)) {
                m_FileParts.emplace_back(buf);
                buf.clear();

                std::string marker = res[1].str();
                if (m_MarkerDict.find(marker) != m_MarkerDict.end()) {
                    std::cout << "  Duplicate marker \"" << marker << "\" in line " << iline << "\n";
                } else {
                    m_MarkerDict[marker] = std::nullopt;
                }

                m_FileParts.emplace_back(MarkerLine{marker, normalizedLine});
            } else {
                buf += normalizedLine;
            }
        }

        m_FileParts.emplace_back(buf);
    }

    /**
     * @brief Sets the replacement text for a marker.
     * @param marker Marker name to set.
     * @param s Replacement text to insert in place of the marker line.
     */
    void setMarker(const std::string& marker, const std::string& s) {
        m_MarkerDict[marker] = s;
    }

    /**
     * @brief Clears all marker replacement texts, reverting to the unset (commented-out) state.
     */
    void resetAllMarkers() {
        for (auto& [k, v] : m_MarkerDict) {
            v = std::nullopt;
        }
    }

    /**
     * @brief Generates an output file from the template, substituting marker replacement texts.
     * @param outfile Path of the file to write.
     * @param markerDict Marker replacement texts to use; if null, the internal marker dict is used.
     */
    void generate(const std::string& outfile, const std::map<std::string, std::optional<std::string>>* markerDict = nullptr) const {
        const std::map<std::string, std::optional<std::string>>& dict = markerDict != nullptr ? *markerDict : m_MarkerDict;

        std::ofstream fout(outfile);
        for (const auto& p : m_FileParts) {
            if (std::holds_alternative<std::string>(p)) {
                fout << std::get<std::string>(p);
            } else {
                const auto& [marker, origLine] = std::get<MarkerLine>(p);

                // original marker line is always written back
                //   1. mark which source is used in generated file
                //   2. harmlessly ignored if not set
                fout << m_CommentString << ' ' << origLine;

                const auto& value = dict.at(marker);
                if (value.has_value()) {
                    fout << *value;
                    fout << '\n'; // ensure a newline after the insertion
                }
            }
        }
    }

private:
    using MarkerLine = std::pair<std::string, std::string>; ///< (marker name, original line)
    using FilePart = std::variant<std::string, MarkerLine>;

    std::vector<FilePart> m_FileParts;
    std::map<std::string, std::optional<std::string>> m_MarkerDict;
    std::string m_TemplateName;
    std::string m_CommentString;
};

} // namespace CuAsm
