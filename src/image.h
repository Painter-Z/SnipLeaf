#pragma once
#include "common.h"
namespace snap
{
struct Image
{
    int width{}, height{};
    std::vector<uint8_t> pixels;
    Image() = default;
    Image(int w, int h, unsigned rgb = 0xffffff);
    bool empty() const
    {
        return width <= 0 || height <= 0 || pixels.empty();
    }
    size_t stride() const
    {
        return static_cast<size_t>(width) * 4;
    }
    Image crop(RECT r) const;
    Image resized(int w, int h) const;
};
struct Dib
{
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ old{};
    uint8_t *bits{};
    int width{}, height{};
    explicit Dib(Image const &image);
    Dib(int w, int h);
    ~Dib();
    Dib(Dib const &) = delete;
    Dib &operator=(Dib const &) = delete;
    Image image() const;
};
Image captureDesktop(RECT &bounds);
Image loadImage(Graphics &g, std::wstring const &path);
void savePng(Graphics &g, Image const &image, std::wstring const &path);
bool copyImage(HWND owner, Graphics &g, Image const &image);
bool copyText(HWND owner, std::wstring const &text);
Image pasteImage(HWND owner, Graphics &g);
Image demoImage(Graphics &g);
void pixelate(Image &image, Box area, int block = 12);
void pixelateDib(Dib &dib, Box area, int block = 12);
} // namespace snap
