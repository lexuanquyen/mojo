// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/resources/picture_pile.h"

#include <algorithm>
#include <limits>
#include <vector>

#include "cc/base/region.h"
#include "cc/resources/picture_pile_impl.h"
#include "cc/resources/raster_worker_pool.h"
#include "skia/ext/analysis_canvas.h"

namespace {
// Layout pixel buffer around the visible layer rect to record.  Any base
// picture that intersects the visible layer rect expanded by this distance
// will be recorded.
const int kPixelDistanceToRecord = 8000;
// We don't perform solid color analysis on images that have more than 10 skia
// operations.
const int kOpCountThatIsOkToAnalyze = 10;

// Dimensions of the tiles in this picture pile as well as the dimensions of
// the base picture in each tile.
const int kBasePictureSize = 512;
const int kTileGridBorderPixels = 1;

// Invalidation frequency settings. kInvalidationFrequencyThreshold is a value
// between 0 and 1 meaning invalidation frequency between 0% and 100% that
// indicates when to stop invalidating offscreen regions.
// kFrequentInvalidationDistanceThreshold defines what it means to be
// "offscreen" in terms of distance to visible in css pixels.
const float kInvalidationFrequencyThreshold = 0.75f;
const int kFrequentInvalidationDistanceThreshold = 512;

// TODO(humper): The density threshold here is somewhat arbitrary; need a
// way to set // this from the command line so we can write a benchmark
// script and find a sweet spot.
const float kDensityThreshold = 0.5f;

bool rect_sort_y(const gfx::Rect& r1, const gfx::Rect& r2) {
  return r1.y() < r2.y() || (r1.y() == r2.y() && r1.x() < r2.x());
}

bool rect_sort_x(const gfx::Rect& r1, const gfx::Rect& r2) {
  return r1.x() < r2.x() || (r1.x() == r2.x() && r1.y() < r2.y());
}

float PerformClustering(const std::vector<gfx::Rect>& tiles,
                        std::vector<gfx::Rect>* clustered_rects) {
  // These variables track the record area and invalid area
  // for the entire clustering
  int total_record_area = 0;
  int total_invalid_area = 0;

  // These variables track the record area and invalid area
  // for the current cluster being constructed.
  gfx::Rect cur_record_rect;
  int cluster_record_area = 0, cluster_invalid_area = 0;

  for (std::vector<gfx::Rect>::const_iterator it = tiles.begin();
        it != tiles.end();
        it++) {
    gfx::Rect invalid_tile = *it;

    // For each tile, we consider adding the invalid tile to the
    // current record rectangle.  Only add it if the amount of empty
    // space created is below a density threshold.
    int tile_area = invalid_tile.width() * invalid_tile.height();

    gfx::Rect proposed_union = cur_record_rect;
    proposed_union.Union(invalid_tile);
    int proposed_area = proposed_union.width() * proposed_union.height();
    float proposed_density =
      static_cast<float>(cluster_invalid_area + tile_area) /
      static_cast<float>(proposed_area);

    if (proposed_density >= kDensityThreshold) {
      // It's okay to add this invalid tile to the
      // current recording rectangle.
      cur_record_rect = proposed_union;
      cluster_record_area = proposed_area;
      cluster_invalid_area += tile_area;
      total_invalid_area += tile_area;
    } else {
      // Adding this invalid tile to the current recording rectangle
      // would exceed our badness threshold, so put the current rectangle
      // in the list of recording rects, and start a new one.
      clustered_rects->push_back(cur_record_rect);
      total_record_area += cluster_record_area;
      cur_record_rect = invalid_tile;
      cluster_invalid_area = tile_area;
      cluster_record_area = tile_area;
    }
  }

  DCHECK(!cur_record_rect.IsEmpty());
  clustered_rects->push_back(cur_record_rect);
  total_record_area += cluster_record_area;;

  DCHECK_NE(total_record_area, 0);

  return static_cast<float>(total_invalid_area) /
         static_cast<float>(total_record_area);
}

void ClusterTiles(const std::vector<gfx::Rect>& invalid_tiles,
                  std::vector<gfx::Rect>* record_rects) {
  TRACE_EVENT1("cc", "ClusterTiles",
               "count",
               invalid_tiles.size());
  if (invalid_tiles.size() <= 1) {
    // Quickly handle the special case for common
    // single-invalidation update, and also the less common
    // case of no tiles passed in.
    *record_rects = invalid_tiles;
    return;
  }

  // Sort the invalid tiles by y coordinate.
  std::vector<gfx::Rect> invalid_tiles_vertical = invalid_tiles;
  std::sort(invalid_tiles_vertical.begin(),
            invalid_tiles_vertical.end(),
            rect_sort_y);

  std::vector<gfx::Rect> vertical_clustering;
  float vertical_density =
      PerformClustering(invalid_tiles_vertical, &vertical_clustering);

  // If vertical density is optimal, then we can return early.
  if (vertical_density == 1.f) {
    *record_rects = vertical_clustering;
    return;
  }

  // Now try again with a horizontal sort, see which one is best
  std::vector<gfx::Rect> invalid_tiles_horizontal = invalid_tiles;
  std::sort(invalid_tiles_horizontal.begin(),
            invalid_tiles_horizontal.end(),
            rect_sort_x);

  std::vector<gfx::Rect> horizontal_clustering;
  float horizontal_density =
      PerformClustering(invalid_tiles_horizontal, &horizontal_clustering);

  if (vertical_density < horizontal_density) {
    *record_rects = horizontal_clustering;
    return;
  }

  *record_rects = vertical_clustering;
}

}  // namespace

namespace cc {

PicturePile::PicturePile()
    : min_contents_scale_(0),
      slow_down_raster_scale_factor_for_debug_(0),
      can_use_lcd_text_(true),
      has_any_recordings_(false),
      is_solid_color_(false),
      solid_color_(SK_ColorTRANSPARENT),
      pixel_record_distance_(kPixelDistanceToRecord),
      is_suitable_for_gpu_rasterization_(true) {
  tiling_.SetMaxTextureSize(gfx::Size(kBasePictureSize, kBasePictureSize));
  tile_grid_info_.fTileInterval.setEmpty();
  tile_grid_info_.fMargin.setEmpty();
  tile_grid_info_.fOffset.setZero();
}

PicturePile::~PicturePile() {
}

bool PicturePile::UpdateAndExpandInvalidation(
    ContentLayerClient* painter,
    Region* invalidation,
    bool can_use_lcd_text,
    const gfx::Size& layer_size,
    const gfx::Rect& visible_layer_rect,
    int frame_number,
    Picture::RecordingMode recording_mode) {
  bool can_use_lcd_text_changed = can_use_lcd_text_ != can_use_lcd_text;
  can_use_lcd_text_ = can_use_lcd_text;

  gfx::Rect interest_rect = visible_layer_rect;
  interest_rect.Inset(-pixel_record_distance_, -pixel_record_distance_);
  recorded_viewport_ = interest_rect;
  recorded_viewport_.Intersect(gfx::Rect(layer_size));

  bool updated =
      ApplyInvalidationAndResize(interest_rect, invalidation, layer_size,
                                 frame_number, can_use_lcd_text_changed);
  std::vector<gfx::Rect> invalid_tiles;
  GetInvalidTileRects(interest_rect, invalidation, visible_layer_rect,
                      frame_number, &invalid_tiles);
  std::vector<gfx::Rect> record_rects;
  ClusterTiles(invalid_tiles, &record_rects);

  if (record_rects.empty())
    return updated;

  CreatePictures(painter, recording_mode, record_rects);

  DetermineIfSolidColor();

  has_any_recordings_ = true;
  DCHECK(CanRasterSlowTileCheck(recorded_viewport_));
  return true;
}

bool PicturePile::ApplyInvalidationAndResize(const gfx::Rect& interest_rect,
                                             Region* invalidation,
                                             const gfx::Size& layer_size,
                                             int frame_number,
                                             bool can_use_lcd_text_changed) {
  bool updated = false;

  Region synthetic_invalidation;
  gfx::Size old_tiling_size = GetSize();
  if (old_tiling_size != layer_size) {
    tiling_.SetTilingSize(layer_size);
    updated = true;
  }
  if (can_use_lcd_text_changed) {
    // When LCD text is enabled/disabled, we must drop any raster tiles for
    // the pile, so they can be recreated in a manner consistent with the new
    // setting. We do this with |synthetic_invalidation| since we don't need to
    // do a new recording, just invalidate rastered content.
    synthetic_invalidation.Union(gfx::Rect(GetSize()));
    updated = true;
  }

  gfx::Rect interest_rect_over_tiles =
      tiling_.ExpandRectToTileBounds(interest_rect);

  if (old_tiling_size != layer_size) {
    gfx::Size min_tiling_size(
        std::min(GetSize().width(), old_tiling_size.width()),
        std::min(GetSize().height(), old_tiling_size.height()));
    gfx::Size max_tiling_size(
        std::max(GetSize().width(), old_tiling_size.width()),
        std::max(GetSize().height(), old_tiling_size.height()));

    has_any_recordings_ = false;

    // Drop recordings that are outside the new or old layer bounds or that
    // changed size.  Newly exposed areas are considered invalidated.
    // Previously exposed areas that are now outside of bounds also need to
    // be invalidated, as they may become part of raster when scale < 1.
    std::vector<PictureMapKey> to_erase;
    int min_toss_x = tiling_.num_tiles_x();
    if (max_tiling_size.width() > min_tiling_size.width()) {
      min_toss_x =
          tiling_.FirstBorderTileXIndexFromSrcCoord(min_tiling_size.width());
    }
    int min_toss_y = tiling_.num_tiles_y();
    if (max_tiling_size.height() > min_tiling_size.height()) {
      min_toss_y =
          tiling_.FirstBorderTileYIndexFromSrcCoord(min_tiling_size.height());
    }
    for (const auto& key_picture_pair : picture_map_) {
      const PictureMapKey& key = key_picture_pair.first;
      if (key.first < min_toss_x && key.second < min_toss_y) {
        has_any_recordings_ |= !!key_picture_pair.second.GetPicture();
        continue;
      }
      to_erase.push_back(key);
    }

    for (size_t i = 0; i < to_erase.size(); ++i)
      picture_map_.erase(to_erase[i]);

    // If a recording is dropped and not re-recorded below, invalidate that
    // full recording to cause any raster tiles that would use it to be
    // dropped.
    // If the recording will be replaced below, invalidate newly exposed
    // areas and previously exposed areas to force raster tiles that include the
    // old recording to know there is new recording to display.
    gfx::Rect min_tiling_rect_over_tiles =
        tiling_.ExpandRectToTileBounds(gfx::Rect(min_tiling_size));
    if (min_toss_x < tiling_.num_tiles_x()) {
      // The bounds which we want to invalidate are the tiles along the old
      // edge of the pile when expanding, or the new edge of the pile when
      // shrinking. In either case, it's the difference of the two, so we'll
      // call this bounding box the DELTA EDGE RECT.
      //
      // In the picture below, the delta edge rect would be the bounding box of
      // tiles {h,i,j}. |min_toss_x| would be equal to the horizontal index of
      // the same tiles.
      //
      //  min pile edge-v  max pile edge-v
      // ---------------+ - - - - - - - -+
      // mmppssvvyybbeeh|h               .
      // mmppssvvyybbeeh|h               .
      // nnqqttwwzzccffi|i               .
      // nnqqttwwzzccffi|i               .
      // oorruuxxaaddggj|j               .
      // oorruuxxaaddggj|j               .
      // ---------------+ - - - - - - - -+ <- min pile edge
      //                                 .
      //  - - - - - - - - - - - - - - - -+ <- max pile edge
      //
      // If you were to slide a vertical beam from the left edge of the
      // delta edge rect toward the right, it would either hit the right edge
      // of the delta edge rect, or the interest rect (expanded to the bounds
      // of the tiles it touches). The same is true for a beam parallel to
      // any of the four edges, sliding across the delta edge rect. We use
      // the union of these four rectangles generated by these beams to
      // determine which part of the delta edge rect is outside of the expanded
      // interest rect.
      //
      // Case 1: Intersect rect is outside the delta edge rect. It can be
      // either on the left or the right. The |left_rect| and |right_rect|,
      // cover this case, one will be empty and one will cover the full
      // delta edge rect. In the picture below, |left_rect| would cover the
      // delta edge rect, and |right_rect| would be empty.
      // +----------------------+ |^^^^^^^^^^^^^^^|
      // |===> DELTA EDGE RECT  | |               |
      // |===>                  | | INTEREST RECT |
      // |===>                  | |               |
      // |===>                  | |               |
      // +----------------------+ |vvvvvvvvvvvvvvv|
      //
      // Case 2: Interest rect is inside the delta edge rect. It will always
      // fill the entire delta edge rect horizontally since the old edge rect
      // is a single tile wide, and the interest rect has been expanded to the
      // bounds of the tiles it touches. In this case the |left_rect| and
      // |right_rect| will be empty, but the case is handled by the |top_rect|
      // and |bottom_rect|. In the picture below, neither the |top_rect| nor
      // |bottom_rect| would empty, they would each cover the area of the old
      // edge rect outside the expanded interest rect.
      // +-----------------+
      // |:::::::::::::::::|
      // |:::::::::::::::::|
      // |vvvvvvvvvvvvvvvvv|
      // |                 |
      // +-----------------+
      // | INTEREST RECT   |
      // |                 |
      // +-----------------+
      // |                 |
      // | DELTA EDGE RECT |
      // +-----------------+
      //
      // Lastly, we need to consider tiles inside the expanded interest rect.
      // For those tiles, we want to invalidate exactly the newly exposed
      // pixels. In the picture below the tiles in the delta edge rect have
      // been resized and the area covered by periods must be invalidated. The
      // |exposed_rect| will cover exactly that area.
      //           v-min pile edge
      // +---------+-------+
      // |         ........|
      // |         ........|
      // | DELTA EDGE.RECT.|
      // |         ........|
      // |         ........|
      // |         ........|
      // |         ........|
      // |         ........|
      // |         ........|
      // +---------+-------+

      int left = tiling_.TilePositionX(min_toss_x);
      int right = left + tiling_.TileSizeX(min_toss_x);
      int top = min_tiling_rect_over_tiles.y();
      int bottom = min_tiling_rect_over_tiles.bottom();

      int left_until = std::min(interest_rect_over_tiles.x(), right);
      int right_until = std::max(interest_rect_over_tiles.right(), left);
      int top_until = std::min(interest_rect_over_tiles.y(), bottom);
      int bottom_until = std::max(interest_rect_over_tiles.bottom(), top);

      int exposed_left = min_tiling_size.width();
      int exposed_left_until = max_tiling_size.width();
      int exposed_top = top;
      int exposed_bottom = max_tiling_size.height();
      DCHECK_GE(exposed_left, left);

      gfx::Rect left_rect(left, top, left_until - left, bottom - top);
      gfx::Rect right_rect(right_until, top, right - right_until, bottom - top);
      gfx::Rect top_rect(left, top, right - left, top_until - top);
      gfx::Rect bottom_rect(
          left, bottom_until, right - left, bottom - bottom_until);
      gfx::Rect exposed_rect(exposed_left,
                             exposed_top,
                             exposed_left_until - exposed_left,
                             exposed_bottom - exposed_top);
      synthetic_invalidation.Union(left_rect);
      synthetic_invalidation.Union(right_rect);
      synthetic_invalidation.Union(top_rect);
      synthetic_invalidation.Union(bottom_rect);
      synthetic_invalidation.Union(exposed_rect);
    }
    if (min_toss_y < tiling_.num_tiles_y()) {
      // The same thing occurs here as in the case above, but the invalidation
      // rect is the bounding box around the bottom row of tiles in the min
      // pile. This would be tiles {o,r,u,x,a,d,g,j} in the above picture.

      int top = tiling_.TilePositionY(min_toss_y);
      int bottom = top + tiling_.TileSizeY(min_toss_y);
      int left = min_tiling_rect_over_tiles.x();
      int right = min_tiling_rect_over_tiles.right();

      int top_until = std::min(interest_rect_over_tiles.y(), bottom);
      int bottom_until = std::max(interest_rect_over_tiles.bottom(), top);
      int left_until = std::min(interest_rect_over_tiles.x(), right);
      int right_until = std::max(interest_rect_over_tiles.right(), left);

      int exposed_top = min_tiling_size.height();
      int exposed_top_until = max_tiling_size.height();
      int exposed_left = left;
      int exposed_right = max_tiling_size.width();
      DCHECK_GE(exposed_top, top);

      gfx::Rect left_rect(left, top, left_until - left, bottom - top);
      gfx::Rect right_rect(right_until, top, right - right_until, bottom - top);
      gfx::Rect top_rect(left, top, right - left, top_until - top);
      gfx::Rect bottom_rect(
          left, bottom_until, right - left, bottom - bottom_until);
      gfx::Rect exposed_rect(exposed_left,
                             exposed_top,
                             exposed_right - exposed_left,
                             exposed_top_until - exposed_top);
      synthetic_invalidation.Union(left_rect);
      synthetic_invalidation.Union(right_rect);
      synthetic_invalidation.Union(top_rect);
      synthetic_invalidation.Union(bottom_rect);
      synthetic_invalidation.Union(exposed_rect);
    }
  }

  // Detect cases where the full pile is invalidated, in this situation we
  // can just drop/invalidate everything.
  if (invalidation->Contains(gfx::Rect(old_tiling_size)) ||
      invalidation->Contains(gfx::Rect(GetSize()))) {
    for (auto& it : picture_map_)
      updated = it.second.Invalidate(frame_number) || updated;
  } else {
    // Expand invalidation that is on tiles that aren't in the interest rect and
    // will not be re-recorded below. These tiles are no longer valid and should
    // be considerered fully invalid, so we can know to not keep around raster
    // tiles that intersect with these recording tiles.
    Region invalidation_expanded_to_full_tiles;

    for (Region::Iterator i(*invalidation); i.has_rect(); i.next()) {
      gfx::Rect invalid_rect = i.rect();

      // This rect covers the bounds (excluding borders) of all tiles whose
      // bounds (including borders) touch the |interest_rect|. This matches
      // the iteration of the |invalid_rect| below which includes borders when
      // calling Invalidate() on pictures.
      gfx::Rect invalid_rect_outside_interest_rect_tiles =
          tiling_.ExpandRectToTileBounds(invalid_rect);
      // We subtract the |interest_rect_over_tiles| which represents the bounds
      // of tiles that will be re-recorded below. This matches the iteration of
      // |interest_rect| below which includes borders.
      // TODO(danakj): We should have a Rect-subtract-Rect-to-2-rects operator
      // instead of using Rect::Subtract which gives you the bounding box of the
      // subtraction.
      invalid_rect_outside_interest_rect_tiles.Subtract(
          interest_rect_over_tiles);
      invalidation_expanded_to_full_tiles.Union(
          invalid_rect_outside_interest_rect_tiles);

      // Split this inflated invalidation across tile boundaries and apply it
      // to all tiles that it touches.
      bool include_borders = true;
      for (TilingData::Iterator iter(&tiling_, invalid_rect, include_borders);
           iter;
           ++iter) {
        const PictureMapKey& key = iter.index();

        PictureMap::iterator picture_it = picture_map_.find(key);
        if (picture_it == picture_map_.end())
          continue;

        // Inform the grid cell that it has been invalidated in this frame.
        updated = picture_it->second.Invalidate(frame_number) || updated;
        // Invalidate drops the picture so the whole tile better be invalidated
        // if it won't be re-recorded below.
        DCHECK_IMPLIES(!tiling_.TileBounds(key.first, key.second)
                            .Intersects(interest_rect_over_tiles),
                       invalidation_expanded_to_full_tiles.Contains(
                           tiling_.TileBounds(key.first, key.second)));
      }
    }
    invalidation->Union(invalidation_expanded_to_full_tiles);
  }

  invalidation->Union(synthetic_invalidation);
  return updated;
}

void PicturePile::GetInvalidTileRects(const gfx::Rect& interest_rect,
                                      Region* invalidation,
                                      const gfx::Rect& visible_layer_rect,
                                      int frame_number,
                                      std::vector<gfx::Rect>* invalid_tiles) {
  // Make a list of all invalid tiles; we will attempt to
  // cluster these into multiple invalidation regions.
  bool include_borders = true;
  for (TilingData::Iterator it(&tiling_, interest_rect, include_borders); it;
       ++it) {
    const PictureMapKey& key = it.index();
    PictureInfo& info = picture_map_[key];

    gfx::Rect rect = PaddedRect(key);
    int distance_to_visible =
        rect.ManhattanInternalDistance(visible_layer_rect);

    if (info.NeedsRecording(frame_number, distance_to_visible)) {
      gfx::Rect tile = tiling_.TileBounds(key.first, key.second);
      invalid_tiles->push_back(tile);
    } else if (!info.GetPicture()) {
      if (recorded_viewport_.Intersects(rect)) {
        // Recorded viewport is just an optimization for a fully recorded
        // interest rect.  In this case, a tile in that rect has declined
        // to be recorded (probably due to frequent invalidations).
        // TODO(enne): Shrink the recorded_viewport_ rather than clearing.
        recorded_viewport_ = gfx::Rect();
      }

      // If a tile in the interest rect is not recorded, the entire tile needs
      // to be considered invalid, so that we know not to keep around raster
      // tiles that intersect this recording tile.
      invalidation->Union(tiling_.TileBounds(it.index_x(), it.index_y()));
    }
  }
}

void PicturePile::CreatePictures(ContentLayerClient* painter,
                                 Picture::RecordingMode recording_mode,
                                 const std::vector<gfx::Rect>& record_rects) {
  for (const auto& record_rect : record_rects) {
    gfx::Rect padded_record_rect = PadRect(record_rect);

    int repeat_count = std::max(1, slow_down_raster_scale_factor_for_debug_);
    scoped_refptr<Picture> picture;

    // Note: Currently, gathering of pixel refs when using a single
    // raster thread doesn't provide any benefit. This might change
    // in the future but we avoid it for now to reduce the cost of
    // Picture::Create.
    bool gather_pixel_refs = RasterWorkerPool::GetNumRasterThreads() > 1;

    for (int i = 0; i < repeat_count; i++) {
      picture = Picture::Create(padded_record_rect, painter, tile_grid_info_,
                                gather_pixel_refs, recording_mode);
      // Note the '&&' with previous is-suitable state.
      // This means that once a picture-pile becomes unsuitable for gpu
      // rasterization due to some content, it will continue to be unsuitable
      // even if that content is replaced by gpu-friendly content.
      // This is an optimization to avoid iterating though all pictures in
      // the pile after each invalidation.
      is_suitable_for_gpu_rasterization_ &=
          picture->IsSuitableForGpuRasterization();
    }

    bool found_tile_for_recorded_picture = false;

    bool include_borders = true;
    for (TilingData::Iterator it(&tiling_, padded_record_rect, include_borders);
         it; ++it) {
      const PictureMapKey& key = it.index();
      gfx::Rect tile = PaddedRect(key);
      if (padded_record_rect.Contains(tile)) {
        PictureInfo& info = picture_map_[key];
        info.SetPicture(picture);
        found_tile_for_recorded_picture = true;
      }
    }
    DCHECK(found_tile_for_recorded_picture);
  }
}

scoped_refptr<RasterSource> PicturePile::CreateRasterSource() const {
  return scoped_refptr<RasterSource>(
      PicturePileImpl::CreateFromPicturePile(this));
}

gfx::Size PicturePile::GetSize() const {
  return tiling_.tiling_size();
}

void PicturePile::SetEmptyBounds() {
  tiling_.SetTilingSize(gfx::Size());
  Clear();
}

void PicturePile::SetMinContentsScale(float min_contents_scale) {
  DCHECK(min_contents_scale);
  if (min_contents_scale_ == min_contents_scale)
    return;

  // Picture contents are played back scaled. When the final contents scale is
  // less than 1 (i.e. low res), then multiple recorded pixels will be used
  // to raster one final pixel.  To avoid splitting a final pixel across
  // pictures (which would result in incorrect rasterization due to blending), a
  // buffer margin is added so that any picture can be snapped to integral
  // final pixels.
  //
  // For example, if a 1/4 contents scale is used, then that would be 3 buffer
  // pixels, since that's the minimum number of pixels to add so that resulting
  // content can be snapped to a four pixel aligned grid.
  int buffer_pixels = static_cast<int>(ceil(1 / min_contents_scale) - 1);
  buffer_pixels = std::max(0, buffer_pixels);
  SetBufferPixels(buffer_pixels);
  min_contents_scale_ = min_contents_scale;
}

void PicturePile::SetSlowdownRasterScaleFactor(int factor) {
  slow_down_raster_scale_factor_for_debug_ = factor;
}

bool PicturePile::IsSuitableForGpuRasterization() const {
  return is_suitable_for_gpu_rasterization_;
}

// static
void PicturePile::ComputeTileGridInfo(const gfx::Size& tile_grid_size,
                                      SkTileGridFactory::TileGridInfo* info) {
  DCHECK(info);
  info->fTileInterval.set(tile_grid_size.width() - 2 * kTileGridBorderPixels,
                          tile_grid_size.height() - 2 * kTileGridBorderPixels);
  DCHECK_GT(info->fTileInterval.width(), 0);
  DCHECK_GT(info->fTileInterval.height(), 0);
  info->fMargin.set(kTileGridBorderPixels, kTileGridBorderPixels);
  // Offset the tile grid coordinate space to take into account the fact
  // that the top-most and left-most tiles do not have top and left borders
  // respectively.
  info->fOffset.set(-kTileGridBorderPixels, -kTileGridBorderPixels);
}

void PicturePile::SetTileGridSize(const gfx::Size& tile_grid_size) {
  ComputeTileGridInfo(tile_grid_size, &tile_grid_info_);
}

void PicturePile::SetUnsuitableForGpuRasterizationForTesting() {
  is_suitable_for_gpu_rasterization_ = false;
}

SkTileGridFactory::TileGridInfo PicturePile::GetTileGridInfoForTesting() const {
  return tile_grid_info_;
}

bool PicturePile::CanRasterSlowTileCheck(const gfx::Rect& layer_rect) const {
  bool include_borders = false;
  for (TilingData::Iterator tile_iter(&tiling_, layer_rect, include_borders);
       tile_iter; ++tile_iter) {
    PictureMap::const_iterator map_iter = picture_map_.find(tile_iter.index());
    if (map_iter == picture_map_.end())
      return false;
    if (!map_iter->second.GetPicture())
      return false;
  }
  return true;
}

void PicturePile::DetermineIfSolidColor() {
  is_solid_color_ = false;
  solid_color_ = SK_ColorTRANSPARENT;

  if (picture_map_.empty()) {
    return;
  }

  PictureMap::const_iterator it = picture_map_.begin();
  const Picture* picture = it->second.GetPicture();

  // Missing recordings due to frequent invalidations or being too far away
  // from the interest rect will cause the a null picture to exist.
  if (!picture)
    return;

  // Don't bother doing more work if the first image is too complicated.
  if (picture->ApproximateOpCount() > kOpCountThatIsOkToAnalyze)
    return;

  // Make sure all of the mapped images point to the same picture.
  for (++it; it != picture_map_.end(); ++it) {
    if (it->second.GetPicture() != picture)
      return;
  }
  skia::AnalysisCanvas canvas(recorded_viewport_.width(),
                              recorded_viewport_.height());
  canvas.translate(-recorded_viewport_.x(), -recorded_viewport_.y());
  picture->Raster(&canvas, nullptr, Region(), 1.0f);
  is_solid_color_ = canvas.GetColorIfSolid(&solid_color_);
}

gfx::Rect PicturePile::PaddedRect(const PictureMapKey& key) const {
  gfx::Rect tile = tiling_.TileBounds(key.first, key.second);
  return PadRect(tile);
}

gfx::Rect PicturePile::PadRect(const gfx::Rect& rect) const {
  gfx::Rect padded_rect = rect;
  padded_rect.Inset(-buffer_pixels(), -buffer_pixels(), -buffer_pixels(),
                    -buffer_pixels());
  return padded_rect;
}

void PicturePile::Clear() {
  picture_map_.clear();
  recorded_viewport_ = gfx::Rect();
  has_any_recordings_ = false;
  is_solid_color_ = false;
}

PicturePile::PictureInfo::PictureInfo() : last_frame_number_(0) {
}

PicturePile::PictureInfo::~PictureInfo() {
}

void PicturePile::PictureInfo::AdvanceInvalidationHistory(int frame_number) {
  DCHECK_GE(frame_number, last_frame_number_);
  if (frame_number == last_frame_number_)
    return;

  invalidation_history_ <<= (frame_number - last_frame_number_);
  last_frame_number_ = frame_number;
}

bool PicturePile::PictureInfo::Invalidate(int frame_number) {
  AdvanceInvalidationHistory(frame_number);
  invalidation_history_.set(0);

  bool did_invalidate = !!picture_.get();
  picture_ = NULL;
  return did_invalidate;
}

bool PicturePile::PictureInfo::NeedsRecording(int frame_number,
                                              int distance_to_visible) {
  AdvanceInvalidationHistory(frame_number);

  // We only need recording if we don't have a picture. Furthermore, we only
  // need a recording if we're within frequent invalidation distance threshold
  // or the invalidation is not frequent enough (below invalidation frequency
  // threshold).
  return !picture_.get() &&
         ((distance_to_visible <= kFrequentInvalidationDistanceThreshold) ||
          (GetInvalidationFrequency() < kInvalidationFrequencyThreshold));
}

void PicturePile::SetBufferPixels(int new_buffer_pixels) {
  if (new_buffer_pixels == buffer_pixels())
    return;

  Clear();
  tiling_.SetBorderTexels(new_buffer_pixels);
}

void PicturePile::PictureInfo::SetPicture(scoped_refptr<Picture> picture) {
  picture_ = picture;
}

const Picture* PicturePile::PictureInfo::GetPicture() const {
  return picture_.get();
}

float PicturePile::PictureInfo::GetInvalidationFrequency() const {
  return invalidation_history_.count() /
         static_cast<float>(INVALIDATION_FRAMES_TRACKED);
}

}  // namespace cc
