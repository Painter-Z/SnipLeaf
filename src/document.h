#pragma once
#include "image.h"
namespace snap
{
enum class Tool
{
    Move,
    Arrow,
    Line,
    Text,
    Mosaic,
    Marker,
    Select,
    Rectangle,
    Ellipse
};
struct Annotation
{
    Tool kind{Tool::Arrow};
    Point from{}, to{};
    std::vector<Point> path;
    std::wstring text;
    unsigned rgb{0xf25f67};
    float width{3.f};
    float fontPt{};      // Zero preserves the size of annotations created with the old width-based format.
    int mosaicPixels{8}; // Square source-pixel cells, independent of stroke width and region aspect ratio.
};
float textFontPoints(Annotation const &mark);
float textFontPixels(Annotation const &mark);
int mosaicCellPixels(Annotation const &mark);
struct RasterLayer
{
    size_t index{};
    Box box;
    std::shared_ptr<const Image> image;
};
struct Token
{
    std::wstring text;
    Box box;
    int line{};
    std::optional<float> angle;
};
struct OcrSnapshot
{
    uint64_t revision{}, epoch{};
    float angle{};
    int width{}, height{};
    std::wstring language, status;
    std::vector<Token> tokens;
    double elapsedMs{};
    unsigned passCount{}, tileCount{};
    bool enhanced{};
    float sourceTextHeight{};
    Point unrotate(Point p) const
    {
        return rotate(p, {width / 2.f, height / 2.f}, -angle);
    }
    int hit(Point p, bool nearest = false, float tolerance = 0.f) const;
    std::wstring selection(int first, int last) const;
    std::vector<Point> quad(Box b) const;
    std::vector<Point> quad(Token const &token) const;
};
struct Document
{
    std::shared_ptr<const Image> base;
    struct Edit
    {
        size_t index{};
        std::optional<Annotation> before, after;
    };
    std::vector<Annotation> marks;
    std::vector<Edit> undoEdits, redoEdits;
    uint64_t revision{1};
    uint64_t flattenCount{};
    std::shared_ptr<const Image> rendered;
    std::shared_ptr<const Image> displayBase;
    std::vector<RasterLayer> rasterLayers;
    explicit Document(Image image) : base(std::make_shared<Image>(std::move(image))), rendered(base), displayBase(base)
    {
    }
    void add(Annotation mark);
    void replace(size_t index, Annotation mark);
    void remove(size_t index);
    void translate(float dx, float dy);
    bool undo();
    bool redo();
    void flatten(Graphics &g, Annotation const *preview = nullptr);
    std::vector<Box> redactions() const;
};
Box annotationBounds(Annotation const &mark);
bool segmentAnnotation(Annotation const &mark);
bool hitAnnotation(Annotation const &mark, Point p, float tolerance);
Annotation transformAnnotation(Annotation const &mark, Box from, Box to);
void measureText(Graphics &g, Annotation &mark, float maxWidth);
void drawAnnotation(Graphics &g, ID2D1RenderTarget *rt, ID2D1SolidColorBrush *brush, ID2D1StrokeStyle *rounded,
                    Annotation const &mark, Box clip);
Image renderAnnotations(Graphics &g, Image const &base, std::span<const Annotation> marks,
                        std::vector<RasterLayer> *rasterLayers = nullptr);
bool isCjk(wchar_t c);
} // namespace snap
