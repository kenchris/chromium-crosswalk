// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_LAYERS_NINE_PATCH_LAYER_H_
#define CC_LAYERS_NINE_PATCH_LAYER_H_

#include "base/memory/scoped_ptr.h"
#include "cc/base/cc_export.h"
#include "cc/layers/layer.h"
#include "cc/resources/ui_resource_client.h"
#include "ui/gfx/rect.h"

namespace cc {

class LayerTreeHost;
class ScopedUIResource;

class CC_EXPORT NinePatchLayer : public Layer {
 public:
  static scoped_refptr<NinePatchLayer> Create();

  virtual bool DrawsContent() const OVERRIDE;

  virtual void PushPropertiesTo(LayerImpl* layer) OVERRIDE;

  virtual void SetLayerTreeHost(LayerTreeHost* host) OVERRIDE;

  // |border| is the space around the center rectangular region in layer space
  // (known as aperture in image space).  |border.x()| and |border.y()| are the
  // size of the left and top boundary, respectively.
  // |border.width()-border.x()| and |border.height()-border.y()| are the size
  // of the right and bottom boundary, respectively.
  void SetBorder(gfx::Rect border);

  // aperture is in the pixel space of the bitmap resource and refers to
  // the center patch of the ninepatch (which is unused in this
  // implementation). We split off eight rects surrounding it and stick them
  // on the edges of the layer. The corners are unscaled, the top and bottom
  // rects are x-stretched to fit, and the left and right rects are
  // y-stretched to fit.
  void SetBitmap(const SkBitmap& skbitmap, gfx::Rect aperture);

  // An alternative way of setting the resource to allow for sharing.
  void SetUIResourceId(UIResourceId resource_id, gfx::Rect aperture);
  void SetFillCenter(bool fill_center);

  class UIResourceHolder {
   public:
    virtual UIResourceId id() = 0;
    virtual ~UIResourceHolder();
  };

 private:
  NinePatchLayer();
  virtual ~NinePatchLayer();
  virtual scoped_ptr<LayerImpl> CreateLayerImpl(LayerTreeImpl* tree_impl)
      OVERRIDE;
  void RecreateUIResourceHolder();

  gfx::Rect border_;
  bool fill_center_;
  scoped_ptr<UIResourceHolder> ui_resource_holder_;
  SkBitmap bitmap_;

  // The transparent center region that shows the parent layer's contents in
  // image space.
  gfx::Rect image_aperture_;

  DISALLOW_COPY_AND_ASSIGN(NinePatchLayer);
};

}  // namespace cc

#endif  // CC_LAYERS_NINE_PATCH_LAYER_H_
