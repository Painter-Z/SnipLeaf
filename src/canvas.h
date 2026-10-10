#pragma once
#include "ocr.h"
namespace snap
{
class App;
class Canvas
{
    App &app_;
    uint64_t id_;
    HWND hwnd_{}, edit_{};
    HFONT editFont_{};
    WNDPROC oldEditProc_{};
    Point textEditorAnchor_{};
    Point textDragStart_{}, textDragAnchor_{};
    bool sizingEditor_{}, textEditorDragging_{}, cropDirty_{};
    std::unique_ptr<Document> doc_;
    std::shared_ptr<Image> desktop_;
    RECT desktopBounds_{}, crop_{};
    bool pinned_{}, showTools_{}, dragging_{}, selecting_{}, moving_{}, panning_{}, cropping_{}, hovering_{},
        closing_{}, releasing_{};
    int resizeEdge_{}, hoverButton_{-1};
    Point start_{}, last_{}, panOrigin_{};
    RECT startWindow_{}, startCrop_{};
    View view_;
    Clock::time_point lastPaint_{};
    uint64_t paintCount_{};
    float ui_{1.f};
    Tool tool_{Tool::Select};
    unsigned ink_{0xf05b61};
    float width_{3.f};
    int mosaicPixels_{8};
    float fontPt_{18.f};
    HWND fontPopup_{}, fontCombo_{};
    HFONT pickerFont_{};
    static LRESULT CALLBACK fontPopupProc(HWND, UINT, WPARAM, LPARAM);
    bool textSizeContext() const;
    bool mosaicSizeContext() const;
    int currentMosaicPixels() const;
    unsigned textEditorInk() const;
    bool editorBorderAt(Point p) const;
    void beginEditorDrag(Point p);
    HBRUSH editorBrush();
    float currentFontPoints() const;
    bool setFontPoints(float points);
    void updateEditorFont();
    void showFontPicker();
    void applyFontPicker();
    void closeFontPicker(bool restoreFocus = true);
    std::optional<Annotation> stroke_;
    int selected_{-1}, objectHandle_{-1}, textIndex_{-1};
    bool objectDragging_{}, objectMoved_{};
    Annotation objectOriginal_;
    Box objectStart_{};
    // A live edit retains the committed document until mouse-up. Later mosaic layers
    // keep opaque tiles above the preview so covered pixels never leak through it.
    std::shared_ptr<const Image> liveBase_, mosaicImage_;
    struct EffectTile
    {
        size_t index;
        Box box;
        std::shared_ptr<const Image> image;
        winrt::com_ptr<ID2D1Bitmap> bitmap;
    };
    std::vector<EffectTile> tailEffects_;
    std::vector<EffectTile> displayEffects_;
    struct EditorBackdrop
    {
        HBRUSH brush{};
        std::unique_ptr<Dib> surface;
        winrt::com_ptr<ID2D1DCRenderTarget> target;
        winrt::com_ptr<ID2D1Bitmap> bitmap, desktopBitmap;
        std::shared_ptr<const Image> source;
        std::vector<EffectTile> effects;
        RECT box{};
        View view;
        bool smooth{}, ready{};
        ~EditorBackdrop()
        {
            if (brush)
                DeleteObject(brush);
        }
    };
    std::unique_ptr<EditorBackdrop> editorBackdrop_;
    uint64_t displayRevision_{};
    bool smoothZoom_{true};
    bool cubicZoomUsed_{};
    void discardTargetBitmaps();
    winrt::com_ptr<ID2D1Bitmap> mosaicBitmap_;
    winrt::com_ptr<ID2D1StrokeStyle> rounded_;
    std::shared_ptr<OcrSnapshot> ocr_;
    uint64_t ocrEpoch_{};
    int anchor_{-1}, caret_{-1};
    std::wstring status_;
    winrt::com_ptr<ID2D1RenderTarget> rt_;
    winrt::com_ptr<ID2D1LinearGradientBrush> pinFrame_;
    winrt::com_ptr<ID2D1Bitmap> bitmap_, backgroundBitmap_;
    struct Button
    {
        int command;
        Box box;
        std::wstring hint;
    };
    std::vector<Button> buttons_;
    Box toolbar_{}, statusBox_{};
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK editProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT message(UINT, WPARAM, LPARAM);
    void layout();
    void paint();
    void drawToolbar(ID2D1SolidColorBrush *);
    void icon(int command, Box b, ID2D1SolidColorBrush *);
    void pointerDown(Point p, WPARAM keys);
    void pointerMove(Point p, WPARAM keys);
    void pointerUp(Point p);
    void key(UINT key, bool ctrl, bool shift);
    void command(int id);
    void commitCrop();
    void changed();
    void refreshBitmap();
    void requestFrame(bool interactive = false);
    void finishText(bool accept);
    void finishGesture();
    void beginText(Point image);
    void sizeTextEditor();
    void editText(int index);
    bool annotationsEditable() const;
    void prepareLive(int index, Tool kind);
    void clearLive();
    void invalidateOcr();
    int objectAt(Point image) const;
    std::vector<Point> objectHandles(Annotation const &) const;
    int handleAt(Point p) const;
    void beginObject(int index, int handle, Point p);
    void cancelGesture();
    Box availableArea() const;
    void fitPin(bool reposition = false);
    void save();
    void copy(bool imageOnly = false);
    void zoom(float factor, Point around);
    void contextMenu(POINT screen);
    Point clampImage(Point p) const;
    int edgeAt(Point p) const;
    int textAt(Point p) const;

  public:
    Canvas(App &, uint64_t, std::unique_ptr<Document> doc, bool showTools = false);
    Canvas(App &, uint64_t, Image desktop, RECT bounds);
    ~Canvas();
    bool create(bool activate = false);
    void close();
    HWND hwnd() const
    {
        return hwnd_;
    }
    uint64_t id() const
    {
        return id_;
    }
    bool pinned() const
    {
        return pinned_;
    }
    void receive(std::shared_ptr<OcrSnapshot> snapshot);
    void requestOcr(bool enhance = false);
    void show(bool visible);
    Document const *document() const
    {
        return doc_.get();
    }
    bool preTranslate(MSG const &message);
    void renderPreview(std::wstring const &path);
    void renderUiPreview(std::wstring const &path);
    friend class App;
};
} // namespace snap
