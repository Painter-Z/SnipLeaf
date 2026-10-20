#pragma once
#include "image.h"
namespace snap
{
// Owned, opaque two-tone cursor: unlike an XOR cursor its colour does not
// change when the screenshot mask is composited underneath it.
class CaptureCursor
{
    HCURSOR cursor_{};
    UINT dpi_{};

  public:
    ~CaptureCursor();
    HCURSOR get(UINT dpi);
    CaptureCursor() = default;
    CaptureCursor(CaptureCursor const &) = delete;
    CaptureCursor &operator=(CaptureCursor const &) = delete;
};
} // namespace snap
