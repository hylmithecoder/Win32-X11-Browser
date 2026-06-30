#include "../include/Debugger.hpp"
#include "../include/Documents.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

int main() {
  int failed = 0;

  // Minimal PDF with one page containing "Hello PDF World"
  const unsigned char kPdf[] =
      "%PDF-1.4\n"
      "1 0 obj\n"
      "<< /Type /Catalog /Pages 2 0 R >>\n"
      "endobj\n"
      "2 0 obj\n"
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>\n"
      "endobj\n"
      "3 0 obj\n"
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]\n"
      "   /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>\n"
      "endobj\n"
      "4 0 obj\n"
      "<< /Length 44 >>\n"
      "stream\n"
      "BT /F1 12 Tf 100 700 Td (Hello PDF World) Tj ET\n"
      "endstream\n"
      "endobj\n"
      "5 0 obj\n"
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\n"
      "endobj\n"
      "xref\n"
      "0 6\n"
      "0000000000 65535 f \n"
      "0000000009 00000 n \n"
      "0000000058 00000 n \n"
      "0000000115 00000 n \n"
      "0000000266 00000 n \n"
      "0000000367 00000 n \n"
      "trailer\n"
      "<< /Size 6 /Root 1 0 R >>\n"
      "startxref\n"
      "445\n"
      "%%EOF\n";

  std::vector<std::uint8_t> data(kPdf, kPdf + sizeof(kPdf) - 1);

  // Test page count
  int pages = DesktopWebview::Documents::pdfPageCount(data);
  if (pages == 1) {
    DEBUG_LOG("  [PASS] pdfPageCount = %d", pages);
  } else {
    DEBUG_LOG("  [FAIL] pdfPageCount expected 1, got %d", pages);
    ++failed;
  }

  // Test render page
  DesktopWebview::Documents::PageBitmap bmp;
  bool ok = DesktopWebview::Documents::renderPdfPage(data, 0, bmp);
  if (ok && bmp.valid() && bmp.width > 0 && bmp.height > 0 &&
      !bmp.pixels.empty()) {
    DEBUG_LOG("  [PASS] renderPdfPage: %dx%d (%zu px)", bmp.width, bmp.height,
              bmp.pixels.size());
  } else {
    DEBUG_LOG("  [FAIL] renderPdfPage returned %s (w=%d h=%d px=%zu)",
              ok ? "true" : "false", bmp.width, bmp.height, bmp.pixels.size());
    ++failed;
  }

  // Test renderPdfToBitmap
  DesktopWebview::Image::Bitmap img;
  ok = DesktopWebview::Documents::renderPdfToBitmap(data, img, 0);
  if (ok && img.valid()) {
    DEBUG_LOG("  [PASS] renderPdfToBitmap: %dx%d", img.width, img.height);
  } else {
    DEBUG_LOG("  [FAIL] renderPdfToBitmap returned %s", ok ? "true" : "false");
    ++failed;
  }

  // Out of range page
  DesktopWebview::Documents::PageBitmap bmp2;
  ok = DesktopWebview::Documents::renderPdfPage(data, 99, bmp2);
  if (!ok && !bmp2.valid()) {
    DEBUG_LOG("  [PASS] out-of-range page rejected");
  } else {
    DEBUG_LOG("  [FAIL] out-of-range page should have failed");
    ++failed;
  }

  // Empty data
  std::vector<std::uint8_t> empty;
  if (DesktopWebview::Documents::pdfPageCount(empty) == 0) {
    DEBUG_LOG("  [PASS] empty data -> 0 pages");
  } else {
    DEBUG_LOG("  [FAIL] empty data should give 0 pages");
    ++failed;
  }

  // Test pdfMetadata (no XMP in minimal PDF, should return empty strings)
  DesktopWebview::Documents::PdfMetadata meta =
      DesktopWebview::Documents::pdfMetadata(data);
  if (meta.title.empty() && meta.author.empty()) {
    DEBUG_LOG("  [PASS] pdfMetadata returns empty for PDF without XMP");
  } else {
    DEBUG_LOG("  [FAIL] pdfMetadata should be empty for minimal PDF");
    ++failed;
  }

  // -------------------------------------------------------------------------
  // Real-file smoke test (optional — skip if file not present)
  // -------------------------------------------------------------------------
  {
    const char *realPath =
        "/srv/http/Abdullah Farras Al Faiq - MODUL 1 PBO UAS.pdf";
    std::ifstream rf(realPath, std::ios::binary);
    if (rf) {
      std::vector<std::uint8_t> rdata((std::istreambuf_iterator<char>(rf)),
                                      std::istreambuf_iterator<char>());
      int rpages = DesktopWebview::Documents::pdfPageCount(rdata);
      DEBUG_LOG("  [INFO] Real PDF: %d pages", rpages);
      if (rpages > 0) {
        DesktopWebview::Documents::PageBitmap rbmp;
        bool rok = DesktopWebview::Documents::renderPdfPage(rdata, 0, rbmp);
        if (rok && rbmp.width > 0 && rbmp.height > 0) {
          DEBUG_LOG("  [PASS] Real PDF page 0 rendered: %dx%d", rbmp.width,
                    rbmp.height);
        } else {
          DEBUG_LOG("  [FAIL] Real PDF page 0 render failed");
          ++failed;
        }
      } else {
        DEBUG_LOG("  [FAIL] Real PDF has 0 pages");
        ++failed;
      }
    } else {
      DEBUG_LOG("  [SKIP] Real PDF not found at %s", realPath);
    }
  }

  printf("\n==========================================================\n");
  if (failed == 0) {
    printf("All PDF tests passed.\n");
  } else {
    printf("%d test(s) FAILED.\n", failed);
  }
  printf("==========================================================\n");
  return failed;
}
