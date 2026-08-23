#pragma once

// Interface only for Phase 1 (spec section 2: "do not implement the complete document
// engine yet"; spec section 35). No .cpp / implementation exists -- a later phase
// provides one (e.g. backed by a Markdown parser + a headless renderer for the PDF path)
// and wires it into engines/CMakeLists.txt.

#include <string>

namespace mediatool::documents {

class IDocumentConverter {
public:
    virtual ~IDocumentConverter() = default;

    virtual bool IsAvailable() const = 0;

    // Each throws errors::MediaToolException on failure (missing input, malformed
    // markup, renderer failure for the PDF path).
    virtual void ConvertMarkdownToText(const std::string& inputPath,
                                       const std::string& outputPath) = 0;

    virtual void ConvertMarkdownToHtml(const std::string& inputPath,
                                       const std::string& outputPath) = 0;

    virtual void ConvertHtmlToText(const std::string& inputPath,
                                   const std::string& outputPath) = 0;

    virtual void ConvertHtmlToPdf(const std::string& inputPath,
                                  const std::string& outputPath) = 0;
};

}  // namespace mediatool::documents
