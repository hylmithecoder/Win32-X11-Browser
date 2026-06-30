#ifndef DOCUMENTS_HPP
#define DOCUMENTS_HPP

#include "Image.hpp"
#include "Paint.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace DesktopWebview {
namespace Documents {

struct PageBitmap {
  int width = 0;
  int height = 0;
  std::vector<Paint::Color> pixels;

  bool valid() const {
    return width > 0 && height > 0 &&
           pixels.size() == static_cast<size_t>(width) * height;
  }
};

struct PdfMetadata {
  std::string title;
  std::string author;
  std::string subject;
  std::string creator;
  std::string producer;
  std::string creationDate;
  std::string modDate;
};

int pdfPageCount(const std::vector<std::uint8_t> &data);

bool renderPdfPage(const std::vector<std::uint8_t> &data, int pageNum,
                   PageBitmap &out);

bool renderPdfToBitmap(const std::vector<std::uint8_t> &data,
                       Image::Bitmap &out, int pageNum = 0);

bool renderAllPdfPages(const std::vector<std::uint8_t> &data,
                       Image::Bitmap &out, double scale = 1.0);

bool pdfPageSizes(const std::vector<std::uint8_t> &data,
                  std::vector<std::pair<double, double>> &sizes);

PdfMetadata pdfMetadata(const std::vector<std::uint8_t> &data);

} // namespace Documents
} // namespace DesktopWebview
#endif // DOCUMENTS_HPP
