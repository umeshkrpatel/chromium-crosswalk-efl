// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/rounded_rect_painter.h"

#include "app/gfx/canvas.h"
#include "app/resource_bundle.h"
#include "third_party/skia/include/effects/SkGradientShader.h"
#include "third_party/skia/include/effects/SkBlurMaskFilter.h"

namespace chromeos {

const SkColor kShadowStrokeColor = SK_ColorLTGRAY;

namespace {

static void DrawRoundedRect(
      gfx::Canvas* canvas,
      int x, int y,
      int w, int h,
      int corner_radius,
      SkColor top_color, SkColor bottom_color,
      SkColor stroke_color) {
  SkRect rect;
  rect.set(
      SkIntToScalar(x), SkIntToScalar(y),
      SkIntToScalar(x + w), SkIntToScalar(y + h));
  SkPath path;
  path.addRoundRect(
      rect, SkIntToScalar(corner_radius), SkIntToScalar(corner_radius));
  SkPaint paint;
  paint.setStyle(SkPaint::kFill_Style);
  paint.setFlags(SkPaint::kAntiAlias_Flag);
  if (top_color != bottom_color) {
    SkPoint p[2];
    p[0].set(SkIntToScalar(x), SkIntToScalar(y));
    p[1].set(SkIntToScalar(x), SkIntToScalar(y + h));
    SkColor colors[2] = { top_color, bottom_color };
    SkShader* s =
        SkGradientShader::CreateLinear(p, colors, NULL, 2,
                                       SkShader::kClamp_TileMode, NULL);
    paint.setShader(s);
    // Need to unref shader, otherwise never deleted.
    s->unref();
  } else {
    paint.setColor(top_color);
  }
  canvas->drawPath(path, paint);

  if (stroke_color != 0) {
    // Expand rect by 0.5px so resulting stroke will take the whole pixel.
    rect.set(
        SkIntToScalar(x) - SK_Scalar1 / 2,
        SkIntToScalar(y) - SK_Scalar1 / 2,
        SkIntToScalar(x + w) + SK_Scalar1 / 2,
        SkIntToScalar(y + h) + SK_Scalar1 / 2);
    paint.setShader(NULL);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(1);
    paint.setColor(stroke_color);
    canvas->drawPath(path, paint);
  }
}

static void DrawRoundedRectShadow(
    gfx::Canvas* canvas,
    int x, int y,
    int w, int h,
    int corner_radius,
    int shadow,
    SkColor color) {
  SkPaint paint;
  paint.setFlags(SkPaint::kAntiAlias_Flag);
  paint.setStyle(SkPaint::kFill_Style);
  paint.setColor(color);
  SkMaskFilter* filter = SkBlurMaskFilter::Create(
      shadow / 2, SkBlurMaskFilter::kNormal_BlurStyle);
  paint.setMaskFilter(filter)->unref();
  SkRect rect;
  rect.set(
      SkIntToScalar(x + shadow / 2), SkIntToScalar(y + shadow / 2),
      SkIntToScalar(x + w - shadow / 2), SkIntToScalar(y + h - shadow / 2));
  canvas->drawRoundRect(
      rect,
      SkIntToScalar(corner_radius), SkIntToScalar(corner_radius),
      paint);
  paint.setMaskFilter(NULL);
}

} //  namespace

RoundedRectPainter::RoundedRectPainter(
    int padding, SkColor padding_color,
    int shadow, SkColor shadow_color,
    int corner_radius,
    SkColor top_color, SkColor bottom_color)
        : padding_(padding), padding_color_(padding_color),
          shadow_(shadow), shadow_color_(shadow_color),
          corner_radius_(corner_radius),
          top_color_(top_color), bottom_color_(bottom_color) {
}

void RoundedRectPainter::Paint(int w, int h, gfx::Canvas* canvas) {
  if (padding_ > 0) {
    SkPaint paint;
    paint.setColor(padding_color_);
    canvas->drawRectCoords(SkIntToScalar(0), SkIntToScalar(0),
                           SkIntToScalar(w), SkIntToScalar(h), paint);
  }
  if (shadow_ > 0) {
    DrawRoundedRectShadow(
        canvas,
        padding_, padding_,
        w - 2 * padding_, h - 2 * padding_,
        corner_radius_,
        shadow_, shadow_color_);
  }
  DrawRoundedRect(
        canvas,
        padding_ + shadow_,
        padding_ + shadow_ - shadow_ / 3,
        w - 2 * padding_ - 2 * shadow_,
        h - 2 * padding_ - 2 * shadow_,
        corner_radius_,
        top_color_, bottom_color_,
        shadow_ ? kShadowStrokeColor : 0);
}

}  // namespace chromeos
