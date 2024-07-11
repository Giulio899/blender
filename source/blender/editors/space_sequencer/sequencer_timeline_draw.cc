/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spseq
 */

#include <cmath>
#include <cstring>

#include "BLI_array.hh"
#include "BLI_blenlib.h"
#include "BLI_string_utils.hh"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_sound_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"

#include "BKE_context.hh"
#include "BKE_fcurve.hh"
#include "BKE_global.hh"
#include "BKE_sound.h"

#include "ED_anim_api.hh"
#include "ED_markers.hh"
#include "ED_mask.hh"
#include "ED_sequencer.hh"
#include "ED_space_api.hh"
#include "ED_time_scrub_ui.hh"

#include "RNA_prototypes.h"

#include "SEQ_channels.hh"
#include "SEQ_effects.hh"
#include "SEQ_prefetch.hh"
#include "SEQ_relations.hh"
#include "SEQ_render.hh"
#include "SEQ_retiming.hh"
#include "SEQ_select.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_time.hh"
#include "SEQ_transform.hh"
#include "SEQ_utils.hh"

#include "UI_interface_icons.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "BLF_api.hh"

#include "MEM_guardedalloc.h"

/* Own include. */
#include "sequencer_intern.hh"
#include "sequencer_quads_batch.hh"
#include "sequencer_strips_batch.hh"

using namespace blender;
using namespace blender::ed::seq;

#define MUTE_ALPHA 120

constexpr float MISSING_ICON_SIZE = 12.0f;

Vector<Sequence *> sequencer_visible_strips_get(const bContext *C)
{
  return sequencer_visible_strips_get(CTX_data_scene(C), UI_view2d_fromcontext(C));
}

Vector<Sequence *> sequencer_visible_strips_get(const Scene *scene, const View2D *v2d)
{
  const Editing *ed = SEQ_editing_get(scene);
  Vector<Sequence *> strips;

  LISTBASE_FOREACH (Sequence *, seq, ed->seqbasep) {
    if (min_ii(SEQ_time_left_handle_frame_get(scene, seq), SEQ_time_start_frame_get(seq)) >
        v2d->cur.xmax)
    {
      continue;
    }
    if (max_ii(SEQ_time_right_handle_frame_get(scene, seq),
               SEQ_time_content_end_frame_get(scene, seq)) < v2d->cur.xmin)
    {
      continue;
    }
    if (seq->machine + 1.0f < v2d->cur.ymin) {
      continue;
    }
    if (seq->machine > v2d->cur.ymax) {
      continue;
    }
    strips.append(seq);
  }
  return strips;
}

static TimelineDrawContext timeline_draw_context_get(const bContext *C, SeqQuadsBatch *quads_batch)
{
  TimelineDrawContext ctx;

  ctx.C = C;
  ctx.region = CTX_wm_region(C);
  ctx.scene = CTX_data_scene(C);
  ctx.sseq = CTX_wm_space_seq(C);
  ctx.v2d = UI_view2d_fromcontext(C);

  ctx.ed = SEQ_editing_get(ctx.scene);
  ctx.channels = ctx.ed ? SEQ_channels_displayed_get(ctx.ed) : nullptr;

  ctx.viewport = WM_draw_region_get_viewport(ctx.region);
  ctx.framebuffer_overlay = GPU_viewport_framebuffer_overlay_get(ctx.viewport);

  ctx.pixely = BLI_rctf_size_y(&ctx.v2d->cur) / BLI_rcti_size_y(&ctx.v2d->mask);
  ctx.pixelx = BLI_rctf_size_x(&ctx.v2d->cur) / BLI_rcti_size_x(&ctx.v2d->mask);

  ctx.retiming_selection = SEQ_retiming_selection_get(ctx.ed);

  ctx.quads = quads_batch;

  return ctx;
}

static bool seq_draw_waveforms_poll(const bContext * /*C*/, SpaceSeq *sseq, Sequence *seq)
{
  const bool strip_is_valid = seq->type == SEQ_TYPE_SOUND_RAM && seq->sound != nullptr;
  const bool overlays_enabled = (sseq->flag & SEQ_SHOW_OVERLAY) != 0;
  const bool ovelay_option = ((sseq->timeline_overlay.flag & SEQ_TIMELINE_ALL_WAVEFORMS) != 0 ||
                              (seq->flag & SEQ_AUDIO_DRAW_WAVEFORM));

  if ((sseq->timeline_overlay.flag & SEQ_TIMELINE_NO_WAVEFORMS) != 0) {
    return false;
  }

  if (strip_is_valid && overlays_enabled && ovelay_option) {
    return true;
  }

  return false;
}

static bool strip_hides_text_overlay_first(TimelineDrawContext *ctx,
                                           const StripDrawContext *strip_ctx)
{
  return seq_draw_waveforms_poll(ctx->C, ctx->sseq, strip_ctx->seq) ||
         strip_ctx->seq->type == SEQ_TYPE_COLOR;
}

static void strip_draw_context_set_text_overlay_visibility(TimelineDrawContext *ctx,
                                                           StripDrawContext *strip_ctx)
{
  float threshold = 8 * UI_SCALE_FAC;
  if (strip_hides_text_overlay_first(ctx, strip_ctx)) {
    threshold = 20 * UI_SCALE_FAC;
  }

  const bool overlays_enabled = (ctx->sseq->timeline_overlay.flag &
                                 (SEQ_TIMELINE_SHOW_STRIP_NAME | SEQ_TIMELINE_SHOW_STRIP_SOURCE |
                                  SEQ_TIMELINE_SHOW_STRIP_DURATION)) != 0;

  strip_ctx->can_draw_text_overlay = (strip_ctx->top - strip_ctx->bottom) / ctx->pixely >=
                                     threshold;
  strip_ctx->can_draw_text_overlay &= overlays_enabled;
}

static void strip_draw_context_set_strip_content_visibility(TimelineDrawContext *ctx,
                                                            StripDrawContext *strip_ctx)
{
  float threshold = 20 * UI_SCALE_FAC;
  if (strip_hides_text_overlay_first(ctx, strip_ctx)) {
    threshold = 8 * UI_SCALE_FAC;
  }

  strip_ctx->can_draw_strip_content = ((strip_ctx->top - strip_ctx->bottom) / ctx->pixely) >
                                      threshold;
}

static StripDrawContext strip_draw_context_get(TimelineDrawContext *ctx, Sequence *seq)
{
  using namespace seq;
  StripDrawContext strip_ctx;
  Scene *scene = ctx->scene;

  strip_ctx.seq = seq;
  strip_ctx.bottom = seq->machine + SEQ_STRIP_OFSBOTTOM;
  strip_ctx.top = seq->machine + SEQ_STRIP_OFSTOP;
  strip_ctx.content_start = SEQ_time_left_handle_frame_get(scene, seq);
  strip_ctx.content_end = SEQ_time_right_handle_frame_get(scene, seq);
  if (SEQ_time_has_left_still_frames(scene, seq)) {
    strip_ctx.content_start = SEQ_time_start_frame_get(seq);
  }
  if (SEQ_time_has_right_still_frames(scene, seq)) {
    strip_ctx.content_end = SEQ_time_content_end_frame_get(scene, seq);
  }
  /* Limit body to strip bounds. Meta strip can end up with content outside of strip range. */
  strip_ctx.content_start = min_ff(strip_ctx.content_start,
                                   SEQ_time_right_handle_frame_get(scene, seq));
  strip_ctx.content_end = max_ff(strip_ctx.content_end,
                                 SEQ_time_left_handle_frame_get(scene, seq));
  strip_ctx.left_handle = SEQ_time_left_handle_frame_get(scene, seq);
  strip_ctx.right_handle = SEQ_time_right_handle_frame_get(scene, seq);
  strip_ctx.strip_length = strip_ctx.right_handle - strip_ctx.left_handle;

  strip_draw_context_set_text_overlay_visibility(ctx, &strip_ctx);
  strip_draw_context_set_strip_content_visibility(ctx, &strip_ctx);
  strip_ctx.strip_is_too_small = (!strip_ctx.can_draw_text_overlay &&
                                  !strip_ctx.can_draw_strip_content);
  strip_ctx.is_active_strip = seq == SEQ_select_active_get(scene);
  strip_ctx.is_single_image = SEQ_transform_single_image_check(seq);
  strip_ctx.handle_width = sequence_handle_size_get_clamped(ctx->scene, seq, ctx->pixelx);
  strip_ctx.show_strip_color_tag = (ctx->sseq->timeline_overlay.flag &
                                    SEQ_TIMELINE_SHOW_STRIP_COLOR_TAG);

  /* Determine if strip (or contents of meta strip) has missing data/media. */
  strip_ctx.missing_data_block = !SEQ_sequence_has_valid_data(seq);
  strip_ctx.missing_media = media_presence_is_missing(scene, seq);
  if (seq->type == SEQ_TYPE_META) {
    const ListBase *seqbase = &seq->seqbase;
    LISTBASE_FOREACH (const Sequence *, sub, seqbase) {
      if (!SEQ_sequence_has_valid_data(sub)) {
        strip_ctx.missing_data_block = true;
      }
      if (media_presence_is_missing(scene, sub)) {
        strip_ctx.missing_media = true;
      }
    }
  }

  if (strip_ctx.can_draw_text_overlay) {
    strip_ctx.strip_content_top = strip_ctx.top - min_ff(0.40f, 20 * UI_SCALE_FAC * ctx->pixely);
  }
  else {
    strip_ctx.strip_content_top = strip_ctx.top;
  }

  return strip_ctx;
}

static void color3ubv_from_seq(const Scene *curscene,
                               const Sequence *seq,
                               const bool show_strip_color_tag,
                               uchar r_col[3])
{
  Editing *ed = SEQ_editing_get(curscene);
  ListBase *channels = SEQ_channels_displayed_get(ed);

  if (show_strip_color_tag && uint(seq->color_tag) < SEQUENCE_COLOR_TOT &&
      seq->color_tag != SEQUENCE_COLOR_NONE)
  {
    bTheme *btheme = UI_GetTheme();
    const ThemeStripColor *strip_color = &btheme->strip_color[seq->color_tag];
    copy_v3_v3_uchar(r_col, strip_color->color);
    return;
  }

  uchar blendcol[3];

  /* Sometimes the active theme is not the sequencer theme, e.g. when an operator invokes the file
   * browser. This makes sure we get the right color values for the theme. */
  bThemeState theme_state;
  UI_Theme_Store(&theme_state);
  UI_SetTheme(SPACE_SEQ, RGN_TYPE_WINDOW);

  switch (seq->type) {
    case SEQ_TYPE_IMAGE:
      UI_GetThemeColor3ubv(TH_SEQ_IMAGE, r_col);
      break;

    case SEQ_TYPE_META:
      UI_GetThemeColor3ubv(TH_SEQ_META, r_col);
      break;

    case SEQ_TYPE_MOVIE:
      UI_GetThemeColor3ubv(TH_SEQ_MOVIE, r_col);
      break;

    case SEQ_TYPE_MOVIECLIP:
      UI_GetThemeColor3ubv(TH_SEQ_MOVIECLIP, r_col);
      break;

    case SEQ_TYPE_MASK:
      UI_GetThemeColor3ubv(TH_SEQ_MASK, r_col);
      break;

    case SEQ_TYPE_SCENE:
      UI_GetThemeColor3ubv(TH_SEQ_SCENE, r_col);

      if (seq->scene == curscene) {
        UI_GetColorPtrShade3ubv(r_col, r_col, 20);
      }
      break;

    /* Transitions use input colors, fallback for when the input is a transition itself. */
    case SEQ_TYPE_CROSS:
    case SEQ_TYPE_GAMCROSS:
    case SEQ_TYPE_WIPE:
      UI_GetThemeColor3ubv(TH_SEQ_TRANSITION, r_col);

      /* Slightly offset hue to distinguish different transition types. */
      if (seq->type == SEQ_TYPE_GAMCROSS) {
        rgb_byte_set_hue_float_offset(r_col, 0.03);
      }
      else if (seq->type == SEQ_TYPE_WIPE) {
        rgb_byte_set_hue_float_offset(r_col, 0.06);
      }
      break;

    /* Effects. */
    case SEQ_TYPE_TRANSFORM:
    case SEQ_TYPE_SPEED:
    case SEQ_TYPE_ADD:
    case SEQ_TYPE_SUB:
    case SEQ_TYPE_MUL:
    case SEQ_TYPE_ALPHAOVER:
    case SEQ_TYPE_ALPHAUNDER:
    case SEQ_TYPE_OVERDROP:
    case SEQ_TYPE_GLOW:
    case SEQ_TYPE_MULTICAM:
    case SEQ_TYPE_ADJUSTMENT:
    case SEQ_TYPE_GAUSSIAN_BLUR:
    case SEQ_TYPE_COLORMIX:
      UI_GetThemeColor3ubv(TH_SEQ_EFFECT, r_col);

      /* Slightly offset hue to distinguish different effects. */
      if (seq->type == SEQ_TYPE_ADD) {
        rgb_byte_set_hue_float_offset(r_col, 0.09);
      }
      else if (seq->type == SEQ_TYPE_SUB) {
        rgb_byte_set_hue_float_offset(r_col, 0.03);
      }
      else if (seq->type == SEQ_TYPE_MUL) {
        rgb_byte_set_hue_float_offset(r_col, 0.06);
      }
      else if (seq->type == SEQ_TYPE_ALPHAOVER) {
        rgb_byte_set_hue_float_offset(r_col, 0.16);
      }
      else if (seq->type == SEQ_TYPE_ALPHAUNDER) {
        rgb_byte_set_hue_float_offset(r_col, 0.19);
      }
      else if (seq->type == SEQ_TYPE_OVERDROP) {
        rgb_byte_set_hue_float_offset(r_col, 0.22);
      }
      else if (seq->type == SEQ_TYPE_COLORMIX) {
        rgb_byte_set_hue_float_offset(r_col, 0.25);
      }
      else if (seq->type == SEQ_TYPE_GAUSSIAN_BLUR) {
        rgb_byte_set_hue_float_offset(r_col, 0.31);
      }
      else if (seq->type == SEQ_TYPE_GLOW) {
        rgb_byte_set_hue_float_offset(r_col, 0.34);
      }
      else if (seq->type == SEQ_TYPE_ADJUSTMENT) {
        rgb_byte_set_hue_float_offset(r_col, 0.89);
      }
      else if (seq->type == SEQ_TYPE_SPEED) {
        rgb_byte_set_hue_float_offset(r_col, 0.72);
      }
      else if (seq->type == SEQ_TYPE_TRANSFORM) {
        rgb_byte_set_hue_float_offset(r_col, 0.75);
      }
      else if (seq->type == SEQ_TYPE_MULTICAM) {
        rgb_byte_set_hue_float_offset(r_col, 0.85);
      }
      break;

    case SEQ_TYPE_COLOR:
      UI_GetThemeColor3ubv(TH_SEQ_COLOR, r_col);
      break;

    case SEQ_TYPE_SOUND_RAM:
      UI_GetThemeColor3ubv(TH_SEQ_AUDIO, r_col);
      blendcol[0] = blendcol[1] = blendcol[2] = 128;
      if (SEQ_render_is_muted(channels, seq)) {
        UI_GetColorPtrBlendShade3ubv(r_col, blendcol, r_col, 0.5, 20);
      }
      break;

    case SEQ_TYPE_TEXT:
      UI_GetThemeColor3ubv(TH_SEQ_TEXT, r_col);
      break;

    default:
      r_col[0] = 10;
      r_col[1] = 255;
      r_col[2] = 40;
      break;
  }

  UI_Theme_Restore(&theme_state);
}

static void waveform_job_start_if_needed(const bContext *C, Sequence *seq)
{
  bSound *sound = seq->sound;

  BLI_spin_lock(static_cast<SpinLock *>(sound->spinlock));
  if (!sound->waveform) {
    /* Load the waveform data if it hasn't been loaded and cached already. */
    if (!(sound->tags & SOUND_TAGS_WAVEFORM_LOADING)) {
      /* Prevent sounds from reloading. */
      sound->tags |= SOUND_TAGS_WAVEFORM_LOADING;
      BLI_spin_unlock(static_cast<SpinLock *>(sound->spinlock));
      sequencer_preview_add_sound(C, seq);
    }
    else {
      BLI_spin_unlock(static_cast<SpinLock *>(sound->spinlock));
    }
  }
  BLI_spin_unlock(static_cast<SpinLock *>(sound->spinlock));
}

static float align_frame_with_pixel(float frame_coord, float frames_per_pixel)
{
  return round_fl_to_int(frame_coord / frames_per_pixel) * frames_per_pixel;
}

static void draw_seq_waveform_overlay(TimelineDrawContext *timeline_ctx,
                                      const StripDrawContext *strip_ctx)
{
  if (!seq_draw_waveforms_poll(timeline_ctx->C, timeline_ctx->sseq, strip_ctx->seq) ||
      strip_ctx->strip_is_too_small)
  {
    return;
  }

  const View2D *v2d = timeline_ctx->v2d;
  Scene *scene = timeline_ctx->scene;
  Sequence *seq = strip_ctx->seq;

  const bool half_style = (timeline_ctx->sseq->timeline_overlay.flag &
                           SEQ_TIMELINE_WAVEFORMS_HALF) != 0;

  const float frames_per_pixel = BLI_rctf_size_x(&v2d->cur) / timeline_ctx->region->winx;
  const float samples_per_frame = SOUND_WAVE_SAMPLES_PER_SECOND / FPS;
  const float samples_per_pixel = samples_per_frame * frames_per_pixel;
  const float bottom = strip_ctx->bottom + timeline_ctx->pixely * 2.0f;
  const float top = strip_ctx->strip_content_top;
  /* The y coordinate of signal level zero. */
  const float y_zero = half_style ? bottom : (bottom + top) / 2.0f;
  /* The y range of unit signal level. */
  const float y_scale = half_style ? top - bottom : (top - bottom) / 2.0f;

  /* Align strip start with nearest pixel to prevent waveform flickering. */
  const float strip_start_aligned = align_frame_with_pixel(
      strip_ctx->left_handle + timeline_ctx->pixelx * 3.0f, frames_per_pixel);
  /* Offset x1 and x2 values, to match view min/max, if strip is out of bounds. */
  const float draw_start_frame = max_ff(v2d->cur.xmin, strip_start_aligned);
  const float draw_end_frame = min_ff(v2d->cur.xmax,
                                      strip_ctx->right_handle - timeline_ctx->pixelx * 3.0f);
  /* Offset must be also aligned, otherwise waveform flickers when moving left handle. */
  float sample_start_frame = draw_start_frame + seq->sound->offset_time / FPS;

  const int pixels_to_draw = round_fl_to_int((draw_end_frame - draw_start_frame) /
                                             frames_per_pixel);

  if (pixels_to_draw < 2) {
    return; /* Not much to draw, exit before running job. */
  }

  waveform_job_start_if_needed(timeline_ctx->C, seq);

  SoundWaveform *waveform = static_cast<SoundWaveform *>(seq->sound->waveform);
  if (waveform == nullptr || waveform->length == 0) {
    return; /* Waveform was not built. */
  }

  /* F-Curve lookup is quite expensive, so do this after precondition. */
  const FCurve *fcu = id_data_find_fcurve(&scene->id, seq, &RNA_Sequence, "volume", 0, nullptr);

  /* Draw zero line (when actual samples close to zero are drawn, they might not cover a pixel. */
  uchar color[4] = {255, 255, 255, 127};
  uchar color_clip[4] = {255, 0, 0, 127};
  uchar color_rms[4] = {255, 255, 255, 204};
  timeline_ctx->quads->add_line(draw_start_frame, y_zero, draw_end_frame, y_zero, color);

  float prev_y_mid = y_zero;
  for (int i = 0; i < pixels_to_draw; i++) {
    float timeline_frame = sample_start_frame + i * frames_per_pixel;
    float frame_index = SEQ_give_frame_index(scene, seq, timeline_frame) + seq->anim_startofs;
    float sample = frame_index * samples_per_frame;
    int sample_index = round_fl_to_int(sample);

    if (sample_index < 0) {
      continue;
    }

    if (sample_index >= waveform->length) {
      break;
    }

    float value_min = waveform->data[sample_index * 3];
    float value_max = waveform->data[sample_index * 3 + 1];
    float rms = waveform->data[sample_index * 3 + 2];

    if (samples_per_pixel > 1.0f) {
      /* We need to sum up the values we skip over until the next step. */
      float next_pos = sample + samples_per_pixel;
      int end_idx = round_fl_to_int(next_pos);

      for (int j = sample_index + 1; (j < waveform->length) && (j < end_idx); j++) {
        value_min = min_ff(value_min, waveform->data[j * 3]);
        value_max = max_ff(value_max, waveform->data[j * 3 + 1]);
        rms = max_ff(rms, waveform->data[j * 3 + 2]);
      }
    }

    float volume = seq->volume;
    if (fcu && !BKE_fcurve_is_empty(fcu)) {
      float evaltime = draw_start_frame + (i * frames_per_pixel);
      volume = evaluate_fcurve(fcu, evaltime);
      CLAMP_MIN(volume, 0.0f);
    }

    value_min *= volume;
    value_max *= volume;
    rms *= volume;

    bool is_clipping = false;
    float clamped_min = clamp_f(value_min, -1.0f, 1.0f);
    float clamped_max = clamp_f(value_max, -1.0f, 1.0f);
    if (clamped_min != value_min || clamped_max != value_max) {
      is_clipping = true;
    }
    value_min = clamped_min;
    value_max = clamped_max;

    /* We are drawing only half to the waveform, mirroring the lower part upwards.
     * If both min and max are on the same side of zero line, we want to draw a bar
     * between them. If min and max cross zero, we want to fill bar from zero to max
     * of those. */
    if (half_style) {
      bool pos_min = value_min > 0.0f;
      bool pos_max = value_max > 0.0f;
      float abs_min = std::abs(value_min);
      float abs_max = std::abs(value_max);
      if (pos_min == pos_max) {
        value_min = std::min(abs_min, abs_max);
        value_max = std::max(abs_min, abs_max);
      }
      else {
        value_min = 0;
        value_max = std::max(abs_min, abs_max);
      }
    }

    float x1 = draw_start_frame + i * frames_per_pixel;
    float x2 = draw_start_frame + (i + 1) * frames_per_pixel;
    float y_min = y_zero + value_min * y_scale;
    float y_max = y_zero + value_max * y_scale;
    float y_mid = (y_max + y_min) * 0.5f;

    /* If a bar would be below 2px, make it a line. */
    if (y_max - y_min < timeline_ctx->pixely * 2) {
      /* If previous segment was also a line of different enough
       * height, join them. */
      if (std::abs(y_mid - prev_y_mid) > timeline_ctx->pixely) {
        float x0 = draw_start_frame + (i - 1) * frames_per_pixel;
        timeline_ctx->quads->add_line(x0, prev_y_mid, x1, y_mid, is_clipping ? color_clip : color);
      }
      timeline_ctx->quads->add_line(x1, y_mid, x2, y_mid, is_clipping ? color_clip : color);
    }
    else {
      float rms_min = y_zero + max_ff(-rms, value_min) * y_scale;
      float rms_max = y_zero + min_ff(rms, value_max) * y_scale;
      /* RMS */
      timeline_ctx->quads->add_quad(
          x1, rms_min, x2, rms_max, is_clipping ? color_clip : color_rms);
      /* Sample */
      timeline_ctx->quads->add_quad(x1, y_min, x2, y_max, is_clipping ? color_clip : color);
    }

    prev_y_mid = y_mid;
  }
}

static void drawmeta_contents(TimelineDrawContext *timeline_ctx,
                              const StripDrawContext *strip_ctx,
                              float corner_radius)
{
  using namespace seq;
  Sequence *seq_meta = strip_ctx->seq;
  if (!strip_ctx->can_draw_strip_content || (timeline_ctx->sseq->flag & SEQ_SHOW_OVERLAY) == 0) {
    return;
  }
  if ((seq_meta->type != SEQ_TYPE_META) &&
      ((seq_meta->type != SEQ_TYPE_SCENE) || (seq_meta->flag & SEQ_SCENE_STRIPS) == 0))
  {
    return;
  }

  Scene *scene = timeline_ctx->scene;

  uchar col[4];

  int chan_min = MAXSEQ;
  int chan_max = 0;
  int chan_range = 0;
  float draw_range = strip_ctx->strip_content_top - strip_ctx->bottom;
  float draw_height;

  Editing *ed = SEQ_editing_get(scene);
  ListBase *channels = SEQ_channels_displayed_get(ed);
  ListBase *meta_seqbase;
  ListBase *meta_channels;
  int offset;

  meta_seqbase = SEQ_get_seqbase_from_sequence(seq_meta, &meta_channels, &offset);

  if (!meta_seqbase || BLI_listbase_is_empty(meta_seqbase)) {
    return;
  }

  if (seq_meta->type == SEQ_TYPE_SCENE) {
    offset = seq_meta->start - offset;
  }
  else {
    offset = 0;
  }

  LISTBASE_FOREACH (Sequence *, seq, meta_seqbase) {
    chan_min = min_ii(chan_min, seq->machine);
    chan_max = max_ii(chan_max, seq->machine);
  }

  chan_range = (chan_max - chan_min) + 1;
  draw_height = draw_range / chan_range;

  col[3] = 196; /* Alpha, used for all meta children. */

  const float meta_x1 = strip_ctx->left_handle + corner_radius * 0.8f * timeline_ctx->pixelx;
  const float meta_x2 = strip_ctx->right_handle - corner_radius * 0.8f * timeline_ctx->pixelx;

  /* Draw only immediate children (1 level depth). */
  LISTBASE_FOREACH (Sequence *, seq, meta_seqbase) {
    float x1_chan = SEQ_time_left_handle_frame_get(scene, seq) + offset;
    float x2_chan = SEQ_time_right_handle_frame_get(scene, seq) + offset;
    if (x1_chan <= meta_x2 && x2_chan >= meta_x1) {
      float y_chan = (seq->machine - chan_min) / float(chan_range) * draw_range;
      float y1_chan, y2_chan;

      if (seq->type == SEQ_TYPE_COLOR) {
        SolidColorVars *colvars = (SolidColorVars *)seq->effectdata;
        rgb_float_to_uchar(col, colvars->col);
      }
      else {
        color3ubv_from_seq(scene, seq, strip_ctx->show_strip_color_tag, col);
      }

      if (SEQ_render_is_muted(channels, seq_meta) || SEQ_render_is_muted(meta_channels, seq)) {
        col[3] = 64;
      }
      else {
        col[3] = 196;
      }

      const bool missing_data = !SEQ_sequence_has_valid_data(seq);
      const bool missing_media = media_presence_is_missing(scene, seq);
      if (missing_data || missing_media) {
        col[0] = 112;
        col[1] = 0;
        col[2] = 0;
      }

      /* Clamp within parent sequence strip bounds. */
      x1_chan = max_ff(x1_chan, meta_x1);
      x2_chan = min_ff(x2_chan, meta_x2);

      y1_chan = strip_ctx->bottom + y_chan + (draw_height * SEQ_STRIP_OFSBOTTOM);
      y2_chan = strip_ctx->bottom + y_chan + (draw_height * SEQ_STRIP_OFSTOP);

      timeline_ctx->quads->add_quad(x1_chan, y1_chan, x2_chan, y2_chan, col);
    }
  }
}

static void draw_handle_transform_text(const TimelineDrawContext *timeline_ctx,
                                       const StripDrawContext *strip_ctx,
                                       eSeqHandle handle)
{
  /* Draw numbers for start and end of the strip next to its handles. */
  if (strip_ctx->strip_is_too_small || (strip_ctx->seq->flag & SELECT) == 0) {
    return;
  }

  if (ED_sequencer_handle_is_selected(strip_ctx->seq, handle) == 0 &&
      (G.moving & G_TRANSFORM_SEQ) == 0)
  {
    return;
  }

  char numstr[64];
  BLF_set_default();

  /* Calculate if strip is wide enough for showing the labels. */
  size_t numstr_len = SNPRINTF_RLEN(
      numstr, "%d%d", int(strip_ctx->left_handle), int(strip_ctx->right_handle));
  const float tot_width = BLF_width(BLF_default(), numstr, numstr_len);

  if (strip_ctx->strip_length / timeline_ctx->pixelx < 20 + tot_width) {
    return;
  }

  const uchar col[4] = {255, 255, 255, 255};
  const float text_margin = 1.2f * strip_ctx->handle_width;
  const float text_y = strip_ctx->bottom + 0.09f;
  float text_x = strip_ctx->left_handle;

  if (handle == SEQ_HANDLE_RIGHT) {
    numstr_len = SNPRINTF_RLEN(numstr, "%d", int(strip_ctx->left_handle));
    text_x += text_margin;
  }
  else {
    numstr_len = SNPRINTF_RLEN(numstr, "%d", int(strip_ctx->right_handle - 1));
    text_x = strip_ctx->right_handle -
             (text_margin + timeline_ctx->pixelx * BLF_width(BLF_default(), numstr, numstr_len));
  }
  UI_view2d_text_cache_add(timeline_ctx->v2d, text_x, text_y, numstr, numstr_len, col);
}

float sequence_handle_size_get_clamped(const Scene *scene, Sequence *seq, const float pixelx)
{
  const bool use_thin_handle = (U.sequencer_editor_flag & USER_SEQ_ED_SIMPLE_TWEAKING) != 0;
  const float handle_size = use_thin_handle ? 5.0f : 8.0f;
  const float maxhandle = (pixelx * handle_size) * U.pixelsize;

  /* Ensure that handle is not wider, than quarter of strip. */
  return min_ff(maxhandle,
                (float(SEQ_time_right_handle_frame_get(scene, seq) -
                       SEQ_time_left_handle_frame_get(scene, seq)) /
                 4.0f));
}

static const char *draw_seq_text_get_name(const Sequence *seq)
{
  const char *name = seq->name + 2;
  if (name[0] == '\0') {
    name = SEQ_sequence_give_name(seq);
  }
  return name;
}

static void draw_seq_text_get_source(const Sequence *seq, char *r_source, size_t source_maxncpy)
{
  *r_source = '\0';

  /* Set source for the most common types. */
  switch (seq->type) {
    case SEQ_TYPE_IMAGE:
    case SEQ_TYPE_MOVIE: {
      BLI_path_join(
          r_source, source_maxncpy, seq->strip->dirpath, seq->strip->stripdata->filename);
      break;
    }
    case SEQ_TYPE_SOUND_RAM: {
      if (seq->sound != nullptr) {
        BLI_strncpy(r_source, seq->sound->filepath, source_maxncpy);
      }
      break;
    }
    case SEQ_TYPE_MULTICAM: {
      BLI_snprintf(r_source, source_maxncpy, "Channel: %d", seq->multicam_source);
      break;
    }
    case SEQ_TYPE_TEXT: {
      const TextVars *textdata = static_cast<TextVars *>(seq->effectdata);
      BLI_strncpy(r_source, textdata->text, source_maxncpy);
      break;
    }
    case SEQ_TYPE_SCENE: {
      if (seq->scene != nullptr) {
        if (seq->scene_camera != nullptr) {
          BLI_snprintf(r_source,
                       source_maxncpy,
                       "%s (%s)",
                       seq->scene->id.name + 2,
                       seq->scene_camera->id.name + 2);
        }
        else {
          BLI_strncpy(r_source, seq->scene->id.name + 2, source_maxncpy);
        }
      }
      break;
    }
    case SEQ_TYPE_MOVIECLIP: {
      if (seq->clip != nullptr) {
        BLI_strncpy(r_source, seq->clip->id.name + 2, source_maxncpy);
      }
      break;
    }
    case SEQ_TYPE_MASK: {
      if (seq->mask != nullptr) {
        BLI_strncpy(r_source, seq->mask->id.name + 2, source_maxncpy);
      }
      break;
    }
  }
}

static size_t draw_seq_text_get_overlay_string(TimelineDrawContext *timeline_ctx,
                                               const StripDrawContext *strip_ctx,
                                               char *r_overlay_string,
                                               size_t overlay_string_len)
{
  const Sequence *seq = strip_ctx->seq;

  const char *text_sep = " | ";
  const char *text_array[5];
  int i = 0;

  if (timeline_ctx->sseq->timeline_overlay.flag & SEQ_TIMELINE_SHOW_STRIP_NAME) {
    text_array[i++] = draw_seq_text_get_name(seq);
  }

  char source[FILE_MAX];
  if (timeline_ctx->sseq->timeline_overlay.flag & SEQ_TIMELINE_SHOW_STRIP_SOURCE) {
    draw_seq_text_get_source(seq, source, sizeof(source));
    if (source[0] != '\0') {
      if (i != 0) {
        text_array[i++] = text_sep;
      }
      text_array[i++] = source;
    }
  }

  char strip_duration_text[16];
  if (timeline_ctx->sseq->timeline_overlay.flag & SEQ_TIMELINE_SHOW_STRIP_DURATION) {
    SNPRINTF(strip_duration_text, "%d", int(strip_ctx->strip_length));
    if (i != 0) {
      text_array[i++] = text_sep;
    }
    text_array[i++] = strip_duration_text;
  }

  BLI_assert(i <= ARRAY_SIZE(text_array));

  return BLI_string_join_array(r_overlay_string, overlay_string_len, text_array, i);
}

static void get_strip_text_color(const TimelineDrawContext *ctx,
                                 const StripDrawContext *strip,
                                 uchar r_col[4])
{
  const Sequence *seq = strip->seq;
  const bool active_or_selected = (seq->flag & SELECT) || strip->is_active_strip;

  /* Text: white when selected/active, black otherwise. */
  r_col[0] = r_col[1] = r_col[2] = r_col[3] = 255;

  /* If not active or selected, draw text black. */
  if (!active_or_selected) {
    r_col[0] = r_col[1] = r_col[2] = 0;

    /* On muted and missing media/data-block strips: gray color, reduce opacity. */
    if ((SEQ_render_is_muted(ctx->channels, seq)) ||
        (strip->missing_data_block || strip->missing_media))
    {
      r_col[0] = r_col[1] = r_col[2] = 192;
      r_col[3] *= 0.66f;
    }
  }
}

static void draw_icon_centered(TimelineDrawContext &ctx,
                               const rctf &rect,
                               int icon_id,
                               const uchar color[4])
{
  UI_view2d_view_ortho(ctx.v2d);
  wmOrtho2_region_pixelspace(ctx.region);

  const float icon_size = 16 * UI_SCALE_FAC;
  if (BLI_rctf_size_x(&ctx.v2d->cur) < icon_size) {
    UI_view2d_view_restore(ctx.C);
    return;
  }

  const float left = ((rect.xmin - ctx.v2d->cur.xmin) / ctx.pixelx);
  const float right = ((rect.xmax - ctx.v2d->cur.xmin) / ctx.pixelx);
  const float bottom = ((rect.ymin - ctx.v2d->cur.ymin) / ctx.pixely);
  const float top = ((rect.ymax - ctx.v2d->cur.ymin) / ctx.pixely);
  const float x_offset = (right - left - icon_size) * 0.5f;
  const float y_offset = (top - bottom - icon_size) * 0.5f;

  UI_icon_draw_ex(left + x_offset,
                  bottom + y_offset,
                  icon_id,
                  UI_INV_SCALE_FAC,
                  1.0f,
                  0.0f,
                  color,
                  false,
                  UI_NO_ICON_OVERLAY_TEXT);

  /* Restore view matrix. */
  UI_view2d_view_restore(ctx.C);
}

static void draw_strip_icons(TimelineDrawContext *timeline_ctx,
                             const Vector<StripDrawContext> &strips)
{
  const float icon_size_x = MISSING_ICON_SIZE * timeline_ctx->pixelx * UI_SCALE_FAC;

  for (const StripDrawContext &strip : strips) {
    const bool missing_data = strip.missing_data_block;
    const bool missing_media = strip.missing_media;
    if (!missing_data && !missing_media) {
      continue;
    }

    /* Draw icon in the title bar area. */
    if ((timeline_ctx->sseq->flag & SEQ_SHOW_OVERLAY) != 0 && !strip.strip_is_too_small &&
        strip.can_draw_text_overlay)
    {
      uchar col[4];
      get_strip_text_color(timeline_ctx, &strip, col);

      float icon_indent = 2.0f * strip.handle_width - 4 * timeline_ctx->pixelx * UI_SCALE_FAC;
      rctf rect;
      rect.ymin = strip.strip_content_top;
      rect.ymax = strip.top;
      rect.xmin = max_ff(strip.left_handle, timeline_ctx->v2d->cur.xmin) + icon_indent;
      if (missing_data) {
        rect.xmax = min_ff(strip.right_handle - strip.handle_width, rect.xmin + icon_size_x);
        draw_icon_centered(*timeline_ctx, rect, ICON_LIBRARY_DATA_BROKEN, col);
        rect.xmin = rect.xmax;
      }
      if (missing_media) {
        rect.xmax = min_ff(strip.right_handle - strip.handle_width, rect.xmin + icon_size_x);
        draw_icon_centered(*timeline_ctx, rect, ICON_ERROR, col);
      }
    }

    /* Draw icon in center of content. */
    if (strip.can_draw_strip_content && strip.seq->type != SEQ_TYPE_META) {
      rctf rect;
      rect.xmin = strip.left_handle + strip.handle_width;
      rect.xmax = strip.right_handle - strip.handle_width;
      rect.ymin = strip.bottom;
      rect.ymax = strip.strip_content_top;
      uchar col[4] = {112, 0, 0, 255};
      if (missing_data) {
        draw_icon_centered(*timeline_ctx, rect, ICON_LIBRARY_DATA_BROKEN, col);
      }
      if (missing_media) {
        draw_icon_centered(*timeline_ctx, rect, ICON_ERROR, col);
      }
    }
  }
}

/* Draw info text on a sequence strip. */
static void draw_seq_text_overlay(TimelineDrawContext *timeline_ctx,
                                  const StripDrawContext *strip_ctx)
{
  if ((timeline_ctx->sseq->flag & SEQ_SHOW_OVERLAY) == 0) {
    return;
  }
  /* Draw text only if there is enough horizontal or vertical space. */
  if ((strip_ctx->strip_length <= 32 * timeline_ctx->pixelx * UI_SCALE_FAC) ||
      strip_ctx->strip_is_too_small || !strip_ctx->can_draw_text_overlay)
  {
    return;
  }

  char overlay_string[FILE_MAX];
  size_t overlay_string_len = draw_seq_text_get_overlay_string(
      timeline_ctx, strip_ctx, overlay_string, sizeof(overlay_string));

  if (overlay_string_len == 0) {
    return;
  }

  uchar col[4];
  get_strip_text_color(timeline_ctx, strip_ctx, col);

  float text_margin = 2.0f * strip_ctx->handle_width;
  rctf rect;
  rect.xmin = strip_ctx->left_handle + text_margin;
  rect.xmax = strip_ctx->right_handle - text_margin;
  rect.ymax = strip_ctx->top;
  /* Depending on the vertical space, draw text on top or in the center of strip. */
  rect.ymin = !strip_ctx->can_draw_strip_content ? strip_ctx->bottom :
                                                   strip_ctx->strip_content_top;
  rect.xmin = max_ff(rect.xmin, timeline_ctx->v2d->cur.xmin + text_margin);
  if (strip_ctx->missing_data_block) {
    rect.xmin += MISSING_ICON_SIZE * timeline_ctx->pixelx * UI_SCALE_FAC;
  }
  if (strip_ctx->missing_media) {
    rect.xmin += MISSING_ICON_SIZE * timeline_ctx->pixelx * UI_SCALE_FAC;
  }
  rect.xmin = min_ff(rect.xmin, timeline_ctx->v2d->cur.xmax);

  CLAMP(rect.xmax, timeline_ctx->v2d->cur.xmin + text_margin, timeline_ctx->v2d->cur.xmax);
  if (rect.xmin >= rect.xmax) { /* No space for label left. */
    return;
  }

  UI_view2d_text_cache_add_rectf(
      timeline_ctx->v2d, &rect, overlay_string, overlay_string_len, col);
}

static void draw_strip_offsets(TimelineDrawContext *timeline_ctx,
                               const StripDrawContext *strip_ctx)
{
  const Sequence *seq = strip_ctx->seq;
  if ((timeline_ctx->sseq->flag & SEQ_SHOW_OVERLAY) == 0) {
    return;
  }
  if (strip_ctx->is_single_image || timeline_ctx->pixely <= 0) {
    return;
  }
  if ((timeline_ctx->sseq->timeline_overlay.flag & SEQ_TIMELINE_SHOW_STRIP_OFFSETS) == 0 &&
      (strip_ctx->seq != ED_sequencer_special_preview_get()))
  {
    return;
  }

  const Scene *scene = timeline_ctx->scene;
  const ListBase *channels = timeline_ctx->channels;

  uchar col[4], blend_col[4];
  color3ubv_from_seq(scene, seq, strip_ctx->show_strip_color_tag, col);
  if (seq->flag & SELECT) {
    UI_GetColorPtrShade3ubv(col, col, 50);
  }
  col[3] = SEQ_render_is_muted(channels, seq) ? MUTE_ALPHA : 200;
  UI_GetColorPtrShade3ubv(col, blend_col, 10);
  blend_col[3] = 255;

  const int strip_start = SEQ_time_start_frame_get(seq);
  const int strip_end = SEQ_time_content_end_frame_get(scene, seq);

  if (strip_ctx->left_handle > strip_start) {
    timeline_ctx->quads->add_quad(strip_start,
                                  strip_ctx->bottom - timeline_ctx->pixely,
                                  strip_ctx->content_start,
                                  strip_ctx->bottom - SEQ_STRIP_OFSBOTTOM,
                                  col);
    timeline_ctx->quads->add_wire_quad(strip_start,
                                       strip_ctx->bottom - timeline_ctx->pixely,
                                       strip_ctx->content_start,
                                       strip_ctx->bottom - SEQ_STRIP_OFSBOTTOM,
                                       blend_col);
  }
  if (strip_ctx->right_handle < strip_end) {
    timeline_ctx->quads->add_quad(strip_ctx->right_handle,
                                  strip_ctx->top + timeline_ctx->pixely,
                                  strip_end,
                                  strip_ctx->top + SEQ_STRIP_OFSBOTTOM,
                                  col);
    timeline_ctx->quads->add_wire_quad(strip_ctx->right_handle,
                                       strip_ctx->top + timeline_ctx->pixely,
                                       strip_end,
                                       strip_ctx->top + SEQ_STRIP_OFSBOTTOM,
                                       blend_col);
  }
}

static uchar mute_alpha_factor_get(const ListBase *channels, const Sequence *seq)
{
  /* Draw muted strips semi-transparent. */
  if (SEQ_render_is_muted(channels, seq)) {
    return MUTE_ALPHA;
  }
  return 255;
}

/**
 * Draw f-curves as darkened regions of the strip:
 * - Volume for sound strips.
 * - Opacity for the other types.
 */
static void draw_seq_fcurve_overlay(TimelineDrawContext *timeline_ctx,
                                    const StripDrawContext *strip_ctx)
{
  if (!strip_ctx->can_draw_strip_content || (timeline_ctx->sseq->flag & SEQ_SHOW_OVERLAY) == 0 ||
      (timeline_ctx->sseq->timeline_overlay.flag & SEQ_TIMELINE_SHOW_FCURVES) == 0)
  {
    return;
  }

  Scene *scene = timeline_ctx->scene;
  const int eval_step = max_ii(1, floor(timeline_ctx->pixelx));
  uchar color[4] = {0, 0, 0, 38};

  const FCurve *fcu;
  if (strip_ctx->seq->type == SEQ_TYPE_SOUND_RAM) {
    fcu = id_data_find_fcurve(&scene->id, strip_ctx->seq, &RNA_Sequence, "volume", 0, nullptr);
  }
  else {
    fcu = id_data_find_fcurve(
        &scene->id, strip_ctx->seq, &RNA_Sequence, "blend_alpha", 0, nullptr);
  }

  if (fcu == nullptr || BKE_fcurve_is_empty(fcu)) {
    return;
  }

  /* Clamp curve evaluation to the editor's borders. */
  int eval_start = max_ff(strip_ctx->left_handle, timeline_ctx->v2d->cur.xmin);
  int eval_end = min_ff(strip_ctx->right_handle, timeline_ctx->v2d->cur.xmax + 1);
  if (eval_start >= eval_end) {
    return;
  }

  const float y_height = strip_ctx->top - strip_ctx->bottom;
  float prev_x = eval_start;
  float prev_val = evaluate_fcurve(fcu, eval_start);
  CLAMP(prev_val, 0.0f, 1.0f);
  bool skip = false;

  for (int timeline_frame = eval_start + eval_step; timeline_frame <= eval_end;
       timeline_frame += eval_step)
  {
    float curve_val = evaluate_fcurve(fcu, timeline_frame);
    CLAMP(curve_val, 0.0f, 1.0f);

    /* Avoid adding adjacent verts that have the same value. */
    if (curve_val == prev_val && timeline_frame < eval_end - eval_step) {
      skip = true;
      continue;
    }

    /* If some frames were skipped above, we need to close the shape. */
    if (skip) {
      timeline_ctx->quads->add_quad(prev_x,
                                    (prev_val * y_height) + strip_ctx->bottom,
                                    prev_x,
                                    strip_ctx->top,
                                    timeline_frame - eval_step,
                                    (prev_val * y_height) + strip_ctx->bottom,
                                    timeline_frame - eval_step,
                                    strip_ctx->top,
                                    color);
      skip = false;
      prev_x = timeline_frame - eval_step;
    }

    timeline_ctx->quads->add_quad(prev_x,
                                  (prev_val * y_height) + strip_ctx->bottom,
                                  prev_x,
                                  strip_ctx->top,
                                  timeline_frame,
                                  (curve_val * y_height) + strip_ctx->bottom,
                                  timeline_frame,
                                  strip_ctx->top,
                                  color);
    prev_x = timeline_frame;
    prev_val = curve_val;
  }
}

/* When active strip is a Multi-cam strip, highlight its source channel. */
static void draw_multicam_highlight(TimelineDrawContext *timeline_ctx,
                                    const StripDrawContext *strip_ctx)
{
  Sequence *act_seq = SEQ_select_active_get(timeline_ctx->scene);

  if (strip_ctx->seq != act_seq || act_seq == nullptr) {
    return;
  }
  if ((act_seq->flag & SELECT) == 0 || act_seq->type != SEQ_TYPE_MULTICAM) {
    return;
  }

  int channel = act_seq->multicam_source;

  if (channel == 0) {
    return;
  }

  View2D *v2d = timeline_ctx->v2d;
  uchar color[4] = {255, 255, 255, 48};
  timeline_ctx->quads->add_quad(v2d->cur.xmin, channel, v2d->cur.xmax, channel + 1, color);
}

/* Force redraw, when prefetching and using cache view. */
static void seq_prefetch_wm_notify(const bContext *C, Scene *scene)
{
  if (SEQ_prefetch_need_redraw(C, scene)) {
    WM_event_add_notifier(C, NC_SCENE | ND_SEQUENCER, nullptr);
  }
}

static void draw_seq_timeline_channels(TimelineDrawContext *ctx)
{
  View2D *v2d = ctx->v2d;
  UI_view2d_view_ortho(v2d);
  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  GPU_blend(GPU_BLEND_ALPHA);
  immUniformThemeColor(TH_ROW_ALTERNATE);

  /* Alternating horizontal stripes. */
  int i = max_ii(1, int(v2d->cur.ymin) - 1);
  while (i < v2d->cur.ymax) {
    if (i & 1) {
      immRectf(pos, v2d->cur.xmin, i, v2d->cur.xmax, i + 1);
    }
    i++;
  }

  GPU_blend(GPU_BLEND_NONE);
  immUnbindProgram();
}

/* Get visible strips into two sets: unselected strips, and selected strips
 * (with selected active being the last in there). This is to make
 * sure that visually selected are always "on top" of others. It matters
 * while selection is being dragged over other strips. */
static void visible_strips_ordered_get(TimelineDrawContext *timeline_ctx,
                                       Vector<StripDrawContext> &r_bottom_layer,
                                       Vector<StripDrawContext> &r_top_layer)
{
  r_bottom_layer.clear();
  r_top_layer.clear();

  Vector<Sequence *> strips = sequencer_visible_strips_get(timeline_ctx->C);

  for (Sequence *seq : strips) {
    StripDrawContext strip_ctx = strip_draw_context_get(timeline_ctx, seq);
    if ((seq->flag & SEQ_OVERLAP) == 0) {
      r_bottom_layer.append(strip_ctx);
    }
    else {
      r_top_layer.append(strip_ctx);
    }
  }
}

static void draw_strips_background(TimelineDrawContext *timeline_ctx,
                                   StripsDrawBatch &strips_batch,
                                   const Vector<StripDrawContext> &strips)
{
  GPU_blend(GPU_BLEND_ALPHA_PREMULT);

  const bool show_overlay = (timeline_ctx->sseq->flag & SEQ_SHOW_OVERLAY) != 0;
  const Scene *scene = timeline_ctx->scene;
  for (const StripDrawContext &strip : strips) {
    SeqStripDrawData &data = strips_batch.add_strip(strip.content_start,
                                                    strip.content_end,
                                                    strip.top,
                                                    strip.bottom,
                                                    strip.strip_content_top,
                                                    strip.left_handle,
                                                    strip.right_handle,
                                                    strip.handle_width,
                                                    strip.is_single_image);

    /* Background color. */
    uchar col[4];
    data.flags |= GPU_SEQ_FLAG_BACKGROUND;
    color3ubv_from_seq(scene, strip.seq, strip.show_strip_color_tag, col);
    col[3] = mute_alpha_factor_get(timeline_ctx->channels, strip.seq);
    /* Muted strips: turn almost gray. */
    if (col[3] == MUTE_ALPHA) {
      uchar muted_color[3] = {128, 128, 128};
      UI_GetColorPtrBlendShade3ubv(col, muted_color, col, 0.5f, 0);
    }
    data.col_background = color_pack(col);

    /* Color band state. */
    if (show_overlay && (strip.seq->type == SEQ_TYPE_COLOR)) {
      data.flags |= GPU_SEQ_FLAG_COLOR_BAND;
      SolidColorVars *colvars = (SolidColorVars *)strip.seq->effectdata;
      rgb_float_to_uchar(col, colvars->col);
      data.col_color_band = color_pack(col);
    }

    /* Transition state. */
    if (show_overlay && strip.can_draw_strip_content &&
        ELEM(strip.seq->type, SEQ_TYPE_CROSS, SEQ_TYPE_GAMCROSS, SEQ_TYPE_WIPE))
    {
      data.flags |= GPU_SEQ_FLAG_TRANSITION;

      const Sequence *seq1 = strip.seq->seq1;
      const Sequence *seq2 = strip.seq->seq2;

      /* Left side. */
      if (seq1->type == SEQ_TYPE_COLOR) {
        rgb_float_to_uchar(col, ((const SolidColorVars *)seq1->effectdata)->col);
      }
      else {
        color3ubv_from_seq(scene, seq1, strip.show_strip_color_tag, col);
      }
      data.col_transition_in = color_pack(col);

      /* Right side. */
      if (seq2->type == SEQ_TYPE_COLOR) {
        rgb_float_to_uchar(col, ((const SolidColorVars *)seq2->effectdata)->col);
      }
      else {
        color3ubv_from_seq(scene, seq2, strip.show_strip_color_tag, col);
        /* If the transition inputs are of the same type, draw the right side slightly darker. */
        if (seq1->type == seq2->type) {
          UI_GetColorPtrShade3ubv(col, col, -15);
        }
      }
      data.col_transition_out = color_pack(col);
    }
  }
  strips_batch.flush_batch();
  GPU_blend(GPU_BLEND_ALPHA);
}

static void strip_data_missing_media_flags_set(const StripDrawContext &strip,
                                               const TimelineDrawContext *timeline_ctx,
                                               SeqStripDrawData &data)
{
  if (strip.missing_data_block || strip.missing_media) {
    /* Do not tint title area for muted strips; we want to see gray for them. */
    if (!SEQ_render_is_muted(timeline_ctx->channels, strip.seq)) {
      data.flags |= GPU_SEQ_FLAG_MISSING_TITLE;
    }
    /* Do not tint content area for meta strips; we want to display children. */
    if (strip.seq->type != SEQ_TYPE_META) {
      data.flags |= GPU_SEQ_FLAG_MISSING_CONTENT;
    }
  }
}

static void strip_data_lock_flags_set(const StripDrawContext &strip,
                                      const TimelineDrawContext *timeline_ctx,
                                      SeqStripDrawData &data)
{
  if (SEQ_transform_is_locked(timeline_ctx->channels, strip.seq)) {
    data.flags |= GPU_SEQ_FLAG_LOCKED;
  }
}

static void strip_data_outline_params_set(const StripDrawContext &strip,
                                          const TimelineDrawContext *timeline_ctx,
                                          SeqStripDrawData &data)
{
  const bool selected = strip.seq->flag & SELECT;
  const bool active = strip.is_active_strip;
  uchar col[4];

  if (selected) {
    UI_GetThemeColor3ubv(active ? TH_SEQ_ACTIVE : TH_SEQ_SELECTED, col);
  }
  else {
    /* Color for unselected strips is a bit darker than the background. */
    UI_GetThemeColorShade3ubv(TH_BACK, -40, col);
  }
  col[3] = 255;
  /* Outline while translating strips:
   *  - Slightly lighter.
   *  - Red when overlapping with other strips. */
  const eSeqOverlapMode overlap_mode = SEQ_tool_settings_overlap_mode_get(timeline_ctx->scene);
  if (G.moving & G_TRANSFORM_SEQ) {
    if ((strip.seq->flag & SEQ_OVERLAP) && (overlap_mode != SEQ_OVERLAP_OVERWRITE)) {
      col[0] = 255;
      col[1] = col[2] = 33;
    }
    else if (selected) {
      UI_GetColorPtrShade3ubv(col, col, 70);
    }
  }

  const bool overlaps = (strip.seq->flag & SEQ_OVERLAP) && (G.moving & G_TRANSFORM_SEQ);
  if (overlaps) {
    data.flags |= GPU_SEQ_FLAG_OVERLAP;
  }

  if (selected) {
    data.flags |= GPU_SEQ_FLAG_SELECTED;
  }
  else if (active && !overlaps) {
    /* If the strips overlap when retiming, don't replace the red outline. */
    /* A subtle highlight outline when active but not selected. */
    UI_GetThemeColorShade3ubv(TH_SEQ_ACTIVE, -40, col);
    data.flags |= GPU_SEQ_FLAG_ACTIVE;
  }
  data.col_outline = color_pack(col);
}

static void strip_data_highlight_flags_set(const StripDrawContext &strip,
                                           const TimelineDrawContext *timeline_ctx,
                                           SeqStripDrawData &data)
{
  const Sequence *act_seq = SEQ_select_active_get(timeline_ctx->scene);
  const Sequence *special_preview = ED_sequencer_special_preview_get();
  /* Highlight if strip is an input of an active strip, or if the strip is solo preview. */
  if (act_seq != nullptr && (act_seq->flag & SELECT) != 0) {
    if (act_seq->seq1 == strip.seq || act_seq->seq2 == strip.seq) {
      data.flags |= GPU_SEQ_FLAG_HIGHLIGHT;
    }
  }
  if (special_preview == strip.seq) {
    data.flags |= GPU_SEQ_FLAG_HIGHLIGHT;
  }
}

static void strip_data_handle_flags_set(const StripDrawContext &strip,
                                        const TimelineDrawContext *timeline_ctx,
                                        SeqStripDrawData &data)
{
  const Scene *scene = timeline_ctx->scene;
  const bool selected = strip.seq->flag & SELECT;
  const bool show_handles = (U.sequencer_editor_flag & USER_SEQ_ED_SIMPLE_TWEAKING) == 0;
  /* Handles on left/right side. */
  if (!SEQ_transform_is_locked(timeline_ctx->channels, strip.seq) &&
      ED_sequencer_can_select_handle(scene, strip.seq, timeline_ctx->v2d))
  {
    const bool selected_l = selected &&
                            ED_sequencer_handle_is_selected(strip.seq, SEQ_HANDLE_LEFT);
    const bool selected_r = selected &&
                            ED_sequencer_handle_is_selected(strip.seq, SEQ_HANDLE_RIGHT);
    const bool show_l = show_handles || selected_l;
    const bool show_r = show_handles || selected_r;
    if (show_l) {
      data.flags |= GPU_SEQ_FLAG_DRAW_LH;
    }
    if (show_r) {
      data.flags |= GPU_SEQ_FLAG_DRAW_RH;
    }
    if (selected_l) {
      data.flags |= GPU_SEQ_FLAG_SELECTED_LH;
    }
    if (selected_r) {
      data.flags |= GPU_SEQ_FLAG_SELECTED_RH;
    }
  }
}

static void draw_strips_foreground(TimelineDrawContext *timeline_ctx,
                                   StripsDrawBatch &strips_batch,
                                   const Vector<StripDrawContext> &strips)
{
  GPU_blend(GPU_BLEND_ALPHA_PREMULT);

  for (const StripDrawContext &strip : strips) {
    SeqStripDrawData &data = strips_batch.add_strip(strip.content_start,
                                                    strip.content_end,
                                                    strip.top,
                                                    strip.bottom,
                                                    strip.strip_content_top,
                                                    strip.left_handle,
                                                    strip.right_handle,
                                                    strip.handle_width,
                                                    strip.is_single_image);
    data.flags |= GPU_SEQ_FLAG_BORDER;
    strip_data_missing_media_flags_set(strip, timeline_ctx, data);
    strip_data_lock_flags_set(strip, timeline_ctx, data);
    strip_data_outline_params_set(strip, timeline_ctx, data);
    strip_data_highlight_flags_set(strip, timeline_ctx, data);
    strip_data_handle_flags_set(strip, timeline_ctx, data);
  }

  strips_batch.flush_batch();
  GPU_blend(GPU_BLEND_ALPHA);
}

static void draw_seq_strips(TimelineDrawContext *timeline_ctx,
                            StripsDrawBatch &strips_batch,
                            const Vector<StripDrawContext> &strips)
{
  if (strips.is_empty()) {
    return;
  }

  UI_view2d_view_ortho(timeline_ctx->v2d);

  /* Draw parts of strips below thumbnails. */
  GPU_blend(GPU_BLEND_ALPHA);
  draw_strips_background(timeline_ctx, strips_batch, strips);

  const float round_radius = calc_strip_round_radius(timeline_ctx->pixely);
  for (const StripDrawContext &strip_ctx : strips) {
    draw_strip_offsets(timeline_ctx, &strip_ctx);
    drawmeta_contents(timeline_ctx, &strip_ctx, round_radius);
  }
  timeline_ctx->quads->draw();

  /* Draw all thumbnails and retiming continuity. */
  GPU_blend(GPU_BLEND_ALPHA);
  for (const StripDrawContext &strip_ctx : strips) {
    draw_seq_strip_thumbnail(timeline_ctx->v2d,
                             timeline_ctx->C,
                             timeline_ctx->scene,
                             strip_ctx.seq,
                             strip_ctx.bottom,
                             strip_ctx.strip_content_top,
                             strip_ctx.top,
                             timeline_ctx->pixelx,
                             timeline_ctx->pixely,
                             round_radius);
    sequencer_retiming_draw_continuity(timeline_ctx, strip_ctx);
  }
  timeline_ctx->quads->draw();

  /* Draw parts of strips above thumbnails. */
  GPU_blend(GPU_BLEND_ALPHA);
  for (const StripDrawContext &strip_ctx : strips) {
    draw_seq_fcurve_overlay(timeline_ctx, &strip_ctx);
    draw_seq_waveform_overlay(timeline_ctx, &strip_ctx);
    draw_multicam_highlight(timeline_ctx, &strip_ctx);
    draw_handle_transform_text(timeline_ctx, &strip_ctx, SEQ_HANDLE_LEFT);
    draw_handle_transform_text(timeline_ctx, &strip_ctx, SEQ_HANDLE_RIGHT);
    draw_seq_text_overlay(timeline_ctx, &strip_ctx);
    sequencer_retiming_keys_draw(timeline_ctx, strip_ctx);
    sequencer_retiming_speed_draw(timeline_ctx, strip_ctx);
  }

  timeline_ctx->quads->draw();

  draw_strips_foreground(timeline_ctx, strips_batch, strips);

  /* Draw icons. */
  draw_strip_icons(timeline_ctx, strips);

  /* Draw text labels. */
  UI_view2d_text_cache_draw(timeline_ctx->region);
  GPU_blend(GPU_BLEND_NONE);
}

static void draw_seq_strips(TimelineDrawContext *timeline_ctx, StripsDrawBatch &strips_batch)
{
  if (timeline_ctx->ed == nullptr) {
    return;
  }

  Vector<StripDrawContext> bottom_layer, top_layer;
  visible_strips_ordered_get(timeline_ctx, bottom_layer, top_layer);
  draw_seq_strips(timeline_ctx, strips_batch, bottom_layer);
  draw_seq_strips(timeline_ctx, strips_batch, top_layer);
}

static void draw_timeline_sfra_efra(TimelineDrawContext *ctx)
{
  const Scene *scene = ctx->scene;
  const View2D *v2d = ctx->v2d;
  const Editing *ed = SEQ_editing_get(scene);
  const int frame_sta = scene->r.sfra;
  const int frame_end = scene->r.efra + 1;

  GPU_blend(GPU_BLEND_ALPHA);

  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Draw overlay outside of frame range. */
  immUniformThemeColorShadeAlpha(TH_BACK, -10, -100);

  if (frame_sta < frame_end) {
    immRectf(pos, v2d->cur.xmin, v2d->cur.ymin, float(frame_sta), v2d->cur.ymax);
    immRectf(pos, float(frame_end), v2d->cur.ymin, v2d->cur.xmax, v2d->cur.ymax);
  }
  else {
    immRectf(pos, v2d->cur.xmin, v2d->cur.ymin, v2d->cur.xmax, v2d->cur.ymax);
  }

  immUniformThemeColorShade(TH_BACK, -60);

  /* Draw frame range boundary. */
  immBegin(GPU_PRIM_LINES, 4);

  immVertex2f(pos, frame_sta, v2d->cur.ymin);
  immVertex2f(pos, frame_sta, v2d->cur.ymax);

  immVertex2f(pos, frame_end, v2d->cur.ymin);
  immVertex2f(pos, frame_end, v2d->cur.ymax);

  immEnd();

  /* While in meta strip, draw a checkerboard overlay outside of frame range. */
  if (ed && !BLI_listbase_is_empty(&ed->metastack)) {
    const MetaStack *ms = static_cast<const MetaStack *>(ed->metastack.last);
    immUnbindProgram();

    immBindBuiltinProgram(GPU_SHADER_2D_CHECKER);

    immUniform4f("color1", 0.0f, 0.0f, 0.0f, 0.22f);
    immUniform4f("color2", 1.0f, 1.0f, 1.0f, 0.0f);
    immUniform1i("size", 8);

    immRectf(pos, v2d->cur.xmin, v2d->cur.ymin, ms->disp_range[0], v2d->cur.ymax);
    immRectf(pos, ms->disp_range[1], v2d->cur.ymin, v2d->cur.xmax, v2d->cur.ymax);

    immUnbindProgram();

    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformThemeColorShade(TH_BACK, -40);

    immBegin(GPU_PRIM_LINES, 4);

    immVertex2f(pos, ms->disp_range[0], v2d->cur.ymin);
    immVertex2f(pos, ms->disp_range[0], v2d->cur.ymax);

    immVertex2f(pos, ms->disp_range[1], v2d->cur.ymin);
    immVertex2f(pos, ms->disp_range[1], v2d->cur.ymax);

    immEnd();
  }

  immUnbindProgram();

  GPU_blend(GPU_BLEND_NONE);
}

struct CacheDrawData {
  const View2D *v2d;
  float stripe_ofs_y;
  float stripe_ht;
  int cache_flag;
  SeqQuadsBatch *quads;
};

/* Called as a callback. */
static bool draw_cache_view_init_fn(void * /*userdata*/, size_t item_count)
{
  return item_count == 0;
}

/* Called as a callback */
static bool draw_cache_view_iter_fn(void *userdata,
                                    Sequence *seq,
                                    int timeline_frame,
                                    int cache_type)
{
  CacheDrawData *drawdata = static_cast<CacheDrawData *>(userdata);
  const View2D *v2d = drawdata->v2d;
  float stripe_top, stripe_bot;

  /* NOTE: Final color is the same as the movie clip cache color.
   * See ED_region_cache_draw_cached_segments.
   */
  const uchar4 col_final{108, 108, 210, 255};
  const uchar4 col_raw{255, 25, 5, 100};
  const uchar4 col_preproc{25, 25, 191, 100};
  const uchar4 col_composite{255, 153, 0, 100};

  uchar4 col{0, 0, 0, 0};

  bool dev_ui = (U.flag & USER_DEVELOPER_UI);

  if ((cache_type & SEQ_CACHE_STORE_FINAL_OUT) &&
      (drawdata->cache_flag & SEQ_CACHE_SHOW_FINAL_OUT))
  {
    /* Draw the final cache on top of the timeline */
    stripe_top = v2d->cur.ymax - (UI_TIME_SCRUB_MARGIN_Y / UI_view2d_scale_get_y(v2d));
    stripe_bot = stripe_top - (UI_TIME_CACHE_MARGIN_Y / UI_view2d_scale_get_y(v2d));
    col = col_final;
  }
  else {
    if (!dev_ui) {
      /* Don't show these cache types below unless developer extras is on. */
      return false;
    }
    if ((cache_type & SEQ_CACHE_STORE_RAW) && (drawdata->cache_flag & SEQ_CACHE_SHOW_RAW)) {
      stripe_bot = seq->machine + SEQ_STRIP_OFSBOTTOM + drawdata->stripe_ofs_y;
      col = col_raw;
    }
    else if ((cache_type & SEQ_CACHE_STORE_PREPROCESSED) &&
             (drawdata->cache_flag & SEQ_CACHE_SHOW_PREPROCESSED))
    {
      stripe_bot = seq->machine + SEQ_STRIP_OFSBOTTOM + drawdata->stripe_ht +
                   drawdata->stripe_ofs_y * 2;
      col = col_preproc;
    }
    else if ((cache_type & SEQ_CACHE_STORE_COMPOSITE) &&
             (drawdata->cache_flag & SEQ_CACHE_SHOW_COMPOSITE))
    {
      stripe_bot = seq->machine + SEQ_STRIP_OFSTOP - drawdata->stripe_ofs_y - drawdata->stripe_ht;
      col = col_composite;
    }
    else {
      return false;
    }
    stripe_top = stripe_bot + drawdata->stripe_ht;
  }

  drawdata->quads->add_quad(timeline_frame, stripe_bot, timeline_frame + 1, stripe_top, col);

  return false;
}

static void draw_cache_stripe(const Scene *scene,
                              const Sequence *seq,
                              SeqQuadsBatch &quads,
                              const float stripe_bot,
                              const float stripe_ht,
                              const uchar color[4])
{
  quads.add_quad(SEQ_time_left_handle_frame_get(scene, seq),
                 stripe_bot,
                 SEQ_time_right_handle_frame_get(scene, seq),
                 stripe_bot + stripe_ht,
                 color);
}

static void draw_cache_background(const bContext *C, CacheDrawData *draw_data)
{
  const Scene *scene = CTX_data_scene(C);
  const View2D *v2d = UI_view2d_fromcontext(C);
  const SpaceSeq *sseq = CTX_wm_space_seq(C);

  /* NOTE: Final bg color is the same as the movie clip cache color.
   * See ED_region_cache_draw_background.
   */
  const uchar4 bg_final{78, 78, 145, 255};
  const uchar4 bg_raw{255, 25, 5, 25};
  const uchar4 bg_preproc{25, 25, 191, 25};
  const uchar4 bg_composite{255, 153, 0, 25};

  float stripe_bot;
  bool dev_ui = (U.flag & USER_DEVELOPER_UI);

  if (sseq->cache_overlay.flag & SEQ_CACHE_SHOW_FINAL_OUT) {
    /* Draw the final cache on top of the timeline */
    float stripe_top = v2d->cur.ymax - (UI_TIME_SCRUB_MARGIN_Y / UI_view2d_scale_get_y(v2d));
    stripe_bot = stripe_top - (UI_TIME_CACHE_MARGIN_Y / UI_view2d_scale_get_y(v2d));

    draw_data->quads->add_quad(scene->r.sfra, stripe_bot, scene->r.efra, stripe_top, bg_final);
  }

  if (!dev_ui) {
    /* Don't show these cache types below unless developer extras is on. */
    return;
  }

  Vector<Sequence *> strips = sequencer_visible_strips_get(C);
  strips.remove_if([&](Sequence *seq) { return seq->type == SEQ_TYPE_SOUND_RAM; });

  for (const Sequence *seq : strips) {
    stripe_bot = seq->machine + SEQ_STRIP_OFSBOTTOM + draw_data->stripe_ofs_y;
    if (sseq->cache_overlay.flag & SEQ_CACHE_SHOW_RAW) {
      draw_cache_stripe(scene, seq, *draw_data->quads, stripe_bot, draw_data->stripe_ht, bg_raw);
    }

    if (sseq->cache_overlay.flag & SEQ_CACHE_SHOW_PREPROCESSED) {
      stripe_bot += draw_data->stripe_ht + draw_data->stripe_ofs_y;
      draw_cache_stripe(
          scene, seq, *draw_data->quads, stripe_bot, draw_data->stripe_ht, bg_preproc);
    }

    if (sseq->cache_overlay.flag & SEQ_CACHE_SHOW_COMPOSITE) {
      stripe_bot = seq->machine + SEQ_STRIP_OFSTOP - draw_data->stripe_ofs_y -
                   draw_data->stripe_ht;
      draw_cache_stripe(
          scene, seq, *draw_data->quads, stripe_bot, draw_data->stripe_ht, bg_composite);
    }
  }
}

static void draw_cache_view(const bContext *C)
{
  Scene *scene = CTX_data_scene(C);
  const View2D *v2d = UI_view2d_fromcontext(C);
  const SpaceSeq *sseq = CTX_wm_space_seq(C);

  if ((sseq->flag & SEQ_SHOW_OVERLAY) == 0 || (sseq->cache_overlay.flag & SEQ_CACHE_SHOW) == 0) {
    return;
  }

  float stripe_ofs_y = UI_view2d_region_to_view_y(v2d, 1.0f) - v2d->cur.ymin;
  float stripe_ht = UI_view2d_region_to_view_y(v2d, 4.0f * UI_SCALE_FAC * U.pixelsize) -
                    v2d->cur.ymin;

  CLAMP_MAX(stripe_ht, 0.2f);
  CLAMP_MIN(stripe_ofs_y, stripe_ht / 2);

  SeqQuadsBatch quads;
  CacheDrawData userdata;
  userdata.v2d = v2d;
  userdata.stripe_ofs_y = stripe_ofs_y;
  userdata.stripe_ht = stripe_ht;
  userdata.cache_flag = sseq->cache_overlay.flag;
  userdata.quads = &quads;

  GPU_blend(GPU_BLEND_ALPHA);

  draw_cache_background(C, &userdata);
  SEQ_cache_iterate(scene, &userdata, draw_cache_view_init_fn, draw_cache_view_iter_fn);

  quads.draw();
  GPU_blend(GPU_BLEND_NONE);
}

/* Draw sequencer timeline. */
static void draw_overlap_frame_indicator(const Scene *scene, const View2D *v2d)
{
  int overlap_frame = (scene->ed->overlay_frame_flag & SEQ_EDIT_OVERLAY_FRAME_ABS) ?
                          scene->ed->overlay_frame_abs :
                          scene->r.cfra + scene->ed->overlay_frame_ofs;

  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR);
  float viewport_size[4];
  GPU_viewport_size_get_f(viewport_size);
  immUniform2f("viewport_size", viewport_size[2], viewport_size[3]);
  /* Shader may have color set from past usage - reset it. */
  immUniform1i("colors_len", 0);
  immUniform1f("dash_width", 20.0f * U.pixelsize);
  immUniform1f("udash_factor", 0.5f);
  immUniformThemeColor(TH_CFRAME);

  immBegin(GPU_PRIM_LINES, 2);
  immVertex2f(pos, overlap_frame, v2d->cur.ymin);
  immVertex2f(pos, overlap_frame, v2d->cur.ymax);
  immEnd();

  immUnbindProgram();
}

static void draw_timeline_grid(TimelineDrawContext *ctx)
{
  if ((ctx->sseq->flag & SEQ_SHOW_OVERLAY) == 0 ||
      (ctx->sseq->timeline_overlay.flag & SEQ_TIMELINE_SHOW_GRID) == 0)
  {
    return;
  }

  U.v2d_min_gridsize *= 3;
  UI_view2d_draw_lines_x__discrete_frames_or_seconds(
      ctx->v2d, ctx->scene, (ctx->sseq->flag & SEQ_DRAWFRAMES) == 0, false);
  U.v2d_min_gridsize /= 3;
}

static void draw_timeline_backdrop(TimelineDrawContext *ctx)
{
  if (ctx->sseq->view != SEQ_VIEW_SEQUENCE || (ctx->sseq->draw_flag & SEQ_DRAW_BACKDROP) == 0) {
    return;
  }

  int preview_frame = ctx->scene->r.cfra;
  if (sequencer_draw_get_transform_preview(ctx->sseq, ctx->scene)) {
    preview_frame = sequencer_draw_get_transform_preview_frame(ctx->scene);
  }

  sequencer_draw_preview(
      ctx->C, ctx->scene, ctx->region, ctx->sseq, preview_frame, 0, false, true);
  UI_view2d_view_ortho(ctx->v2d);
}

static void draw_timeline_markers(TimelineDrawContext *ctx)
{
  if ((ctx->sseq->flag & SEQ_SHOW_MARKERS) == 0) {
    return;
  }

  UI_view2d_view_orthoSpecial(ctx->region, ctx->v2d, true);
  ED_markers_draw(ctx->C, DRAW_MARKERS_MARGIN);
}

static void draw_timeline_gizmos(TimelineDrawContext *ctx)
{
  if ((ctx->sseq->gizmo_flag & SEQ_GIZMO_HIDE) != 0) {
    return;
  }

  WM_gizmomap_draw(ctx->region->gizmo_map, ctx->C, WM_GIZMOMAP_DRAWSTEP_2D);
}

static void draw_timeline_pre_view_callbacks(TimelineDrawContext *ctx)
{
  GPU_framebuffer_bind_no_srgb(ctx->framebuffer_overlay);
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_framebuffer_bind(ctx->framebuffer_overlay);
  ED_region_draw_cb_draw(ctx->C, ctx->region, REGION_DRAW_PRE_VIEW);
  GPU_framebuffer_bind_no_srgb(ctx->framebuffer_overlay);
}

static void draw_timeline_post_view_callbacks(TimelineDrawContext *ctx)
{
  GPU_framebuffer_bind(ctx->framebuffer_overlay);
  ED_region_draw_cb_draw(ctx->C, ctx->region, REGION_DRAW_POST_VIEW);
  GPU_framebuffer_bind_no_srgb(ctx->framebuffer_overlay);
}

void draw_timeline_seq(const bContext *C, ARegion *region)
{
  SeqQuadsBatch quads_batch;
  TimelineDrawContext ctx = timeline_draw_context_get(C, &quads_batch);
  StripsDrawBatch strips_batch(ctx.pixelx, ctx.pixely);

  draw_timeline_pre_view_callbacks(&ctx);
  UI_ThemeClearColor(TH_BACK);
  draw_seq_timeline_channels(&ctx);
  draw_timeline_grid(&ctx);
  draw_timeline_backdrop(&ctx);
  draw_timeline_sfra_efra(&ctx);
  draw_seq_strips(&ctx, strips_batch);
  draw_timeline_markers(&ctx);
  UI_view2d_view_ortho(ctx.v2d);
  ANIM_draw_previewrange(C, ctx.v2d, 1);
  draw_timeline_gizmos(&ctx);
  draw_timeline_post_view_callbacks(&ctx);
  ED_time_scrub_draw(region, ctx.scene, !(ctx.sseq->flag & SEQ_DRAWFRAMES), true);

  seq_prefetch_wm_notify(C, ctx.scene);
}

void draw_timeline_seq_display(const bContext *C, ARegion *region)
{
  const Scene *scene = CTX_data_scene(C);
  const SpaceSeq *sseq = CTX_wm_space_seq(C);
  View2D *v2d = &region->v2d;

  if (scene->ed != nullptr) {
    UI_view2d_view_ortho(v2d);
    draw_cache_view(C);
    if (scene->ed->overlay_frame_flag & SEQ_EDIT_OVERLAY_FRAME_SHOW) {
      draw_overlap_frame_indicator(scene, v2d);
    }
    UI_view2d_view_restore(C);
  }

  ED_time_scrub_draw_current_frame(region, scene, !(sseq->flag & SEQ_DRAWFRAMES));

  const ListBase *seqbase = SEQ_active_seqbase_get(SEQ_editing_get(scene));
  SEQ_timeline_boundbox(scene, seqbase, &v2d->tot);
  const rcti scroller_mask = ED_time_scrub_clamp_scroller_mask(v2d->mask);
  UI_view2d_scrollers_draw(v2d, &scroller_mask);
}
