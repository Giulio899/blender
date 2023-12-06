/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2003-2009 Blender Authors
 * SPDX-FileCopyrightText: 2005-2006 Peter Schlaile <peter [at] schlaile [dot] de>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_path_util.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

#include "DNA_anim_types.h"
#include "DNA_packedFile_types.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_space_types.h"
#include "DNA_vfont_types.h"

#include "BKE_fcurve.h"
#include "BKE_lib_id.h"
#include "BKE_main.hh"

#include "IMB_colormanagement.h"
#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"
#include "IMB_metadata.h"

#include "BLI_math_color_blend.h"

#include "RNA_access.hh"
#include "RNA_prototypes.h"

#include "RE_pipeline.h"

#include "SEQ_channels.hh"
#include "SEQ_effects.hh"
#include "SEQ_proxy.hh"
#include "SEQ_relations.hh"
#include "SEQ_render.hh"
#include "SEQ_time.hh"
#include "SEQ_utils.hh"

#include "BLF_api.h"

#include "effects.hh"
#include "render.hh"
#include "strip_time.hh"
#include "utils.hh"

static SeqEffectHandle get_sequence_effect_impl(int seq_type);

/* -------------------------------------------------------------------- */
/** \name Internal Utilities
 * \{ */

static void slice_get_byte_buffers(const SeqRenderData *context,
                                   const ImBuf *ibuf1,
                                   const ImBuf *ibuf2,
                                   const ImBuf *ibuf3,
                                   const ImBuf *out,
                                   int start_line,
                                   uchar **rect1,
                                   uchar **rect2,
                                   uchar **rect3,
                                   uchar **rect_out)
{
  int offset = 4 * start_line * context->rectx;

  *rect1 = ibuf1->byte_buffer.data + offset;
  *rect_out = out->byte_buffer.data + offset;

  if (ibuf2) {
    *rect2 = ibuf2->byte_buffer.data + offset;
  }

  if (ibuf3) {
    *rect3 = ibuf3->byte_buffer.data + offset;
  }
}

static void slice_get_float_buffers(const SeqRenderData *context,
                                    const ImBuf *ibuf1,
                                    const ImBuf *ibuf2,
                                    const ImBuf *ibuf3,
                                    const ImBuf *out,
                                    int start_line,
                                    float **rect1,
                                    float **rect2,
                                    float **rect3,
                                    float **rect_out)
{
  int offset = 4 * start_line * context->rectx;

  *rect1 = ibuf1->float_buffer.data + offset;
  *rect_out = out->float_buffer.data + offset;

  if (ibuf2) {
    *rect2 = ibuf2->float_buffer.data + offset;
  }

  if (ibuf3) {
    *rect3 = ibuf3->float_buffer.data + offset;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glow Effect
 * \{ */

static ImBuf *prepare_effect_imbufs(const SeqRenderData *context,
                                    ImBuf *ibuf1,
                                    ImBuf *ibuf2,
                                    ImBuf *ibuf3)
{
  ImBuf *out;
  Scene *scene = context->scene;
  int x = context->rectx;
  int y = context->recty;

  if (!ibuf1 && !ibuf2 && !ibuf3) {
    /* hmmm, global float option ? */
    out = IMB_allocImBuf(x, y, 32, IB_rect);
  }
  else if ((ibuf1 && ibuf1->float_buffer.data) || (ibuf2 && ibuf2->float_buffer.data) ||
           (ibuf3 && ibuf3->float_buffer.data))
  {
    /* if any inputs are rectfloat, output is float too */

    out = IMB_allocImBuf(x, y, 32, IB_rectfloat);
  }
  else {
    out = IMB_allocImBuf(x, y, 32, IB_rect);
  }

  if (out->float_buffer.data) {
    if (ibuf1 && !ibuf1->float_buffer.data) {
      seq_imbuf_to_sequencer_space(scene, ibuf1, true);
    }

    if (ibuf2 && !ibuf2->float_buffer.data) {
      seq_imbuf_to_sequencer_space(scene, ibuf2, true);
    }

    if (ibuf3 && !ibuf3->float_buffer.data) {
      seq_imbuf_to_sequencer_space(scene, ibuf3, true);
    }

    IMB_colormanagement_assign_float_colorspace(out, scene->sequencer_colorspace_settings.name);
  }
  else {
    if (ibuf1 && !ibuf1->byte_buffer.data) {
      IMB_rect_from_float(ibuf1);
    }

    if (ibuf2 && !ibuf2->byte_buffer.data) {
      IMB_rect_from_float(ibuf2);
    }

    if (ibuf3 && !ibuf3->byte_buffer.data) {
      IMB_rect_from_float(ibuf3);
    }
  }

  /* If effect only affecting a single channel, forward input's metadata to the output. */
  if (ibuf1 != nullptr && ibuf1 == ibuf2 && ibuf2 == ibuf3) {
    IMB_metadata_copy(out, ibuf1);
  }

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Alpha Over Effect
 * \{ */

static void init_alpha_over_or_under(Sequence *seq)
{
  Sequence *seq1 = seq->seq1;
  Sequence *seq2 = seq->seq2;

  seq->seq2 = seq1;
  seq->seq1 = seq2;
}

static void do_alphaover_effect_byte(
    float fac, int x, int y, uchar *rect1, uchar *rect2, uchar *out)
{
  uchar *cp1 = rect1;
  uchar *cp2 = rect2;
  uchar *rt = out;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      /* rt = rt1 over rt2  (alpha from rt1) */

      float tempc[4], rt1[4], rt2[4];
      straight_uchar_to_premul_float(rt1, cp1);
      straight_uchar_to_premul_float(rt2, cp2);

      float mfac = 1.0f - fac * rt1[3];

      if (fac <= 0.0f) {
        *((uint *)rt) = *((uint *)cp2);
      }
      else if (mfac <= 0.0f) {
        *((uint *)rt) = *((uint *)cp1);
      }
      else {
        tempc[0] = fac * rt1[0] + mfac * rt2[0];
        tempc[1] = fac * rt1[1] + mfac * rt2[1];
        tempc[2] = fac * rt1[2] + mfac * rt2[2];
        tempc[3] = fac * rt1[3] + mfac * rt2[3];

        premul_float_to_straight_uchar(rt, tempc);
      }
      cp1 += 4;
      cp2 += 4;
      rt += 4;
    }
  }
}

static void do_alphaover_effect_float(
    float fac, int x, int y, float *rect1, float *rect2, float *out)
{
  float *rt1 = rect1;
  float *rt2 = rect2;
  float *rt = out;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      /* rt = rt1 over rt2  (alpha from rt1) */

      float mfac = 1.0f - (fac * rt1[3]);

      if (fac <= 0.0f) {
        memcpy(rt, rt2, sizeof(float[4]));
      }
      else if (mfac <= 0) {
        memcpy(rt, rt1, sizeof(float[4]));
      }
      else {
        rt[0] = fac * rt1[0] + mfac * rt2[0];
        rt[1] = fac * rt1[1] + mfac * rt2[1];
        rt[2] = fac * rt1[2] + mfac * rt2[2];
        rt[3] = fac * rt1[3] + mfac * rt2[3];
      }
      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_alphaover_effect(const SeqRenderData *context,
                                Sequence * /*seq*/,
                                float /*timeline_frame*/,
                                float fac,
                                ImBuf *ibuf1,
                                ImBuf *ibuf2,
                                ImBuf * /*ibuf3*/,
                                int start_line,
                                int total_lines,
                                ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_alphaover_effect_float(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_alphaover_effect_byte(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Alpha Under Effect
 * \{ */

static void do_alphaunder_effect_byte(
    float fac, int x, int y, uchar *rect1, uchar *rect2, uchar *out)
{
  uchar *cp1 = rect1;
  uchar *cp2 = rect2;
  uchar *rt = out;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      /* rt = rt1 under rt2  (alpha from rt2) */

      float tempc[4], rt1[4], rt2[4];
      straight_uchar_to_premul_float(rt1, cp1);
      straight_uchar_to_premul_float(rt2, cp2);

      /* this complex optimization is because the
       * 'skybuf' can be crossed in
       */
      if (rt2[3] <= 0.0f && fac >= 1.0f) {
        *((uint *)rt) = *((uint *)cp1);
      }
      else if (rt2[3] >= 1.0f) {
        *((uint *)rt) = *((uint *)cp2);
      }
      else {
        float temp_fac = (fac * (1.0f - rt2[3]));

        if (fac <= 0) {
          *((uint *)rt) = *((uint *)cp2);
        }
        else {
          tempc[0] = (temp_fac * rt1[0] + rt2[0]);
          tempc[1] = (temp_fac * rt1[1] + rt2[1]);
          tempc[2] = (temp_fac * rt1[2] + rt2[2]);
          tempc[3] = (temp_fac * rt1[3] + rt2[3]);

          premul_float_to_straight_uchar(rt, tempc);
        }
      }
      cp1 += 4;
      cp2 += 4;
      rt += 4;
    }
  }
}

static void do_alphaunder_effect_float(
    float fac, int x, int y, float *rect1, float *rect2, float *out)
{
  float *rt1 = rect1;
  float *rt2 = rect2;
  float *rt = out;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      /* rt = rt1 under rt2  (alpha from rt2) */

      /* this complex optimization is because the
       * 'skybuf' can be crossed in
       */
      if (rt2[3] <= 0 && fac >= 1.0f) {
        memcpy(rt, rt1, sizeof(float[4]));
      }
      else if (rt2[3] >= 1.0f) {
        memcpy(rt, rt2, sizeof(float[4]));
      }
      else {
        float temp_fac = fac * (1.0f - rt2[3]);

        if (fac == 0) {
          memcpy(rt, rt2, sizeof(float[4]));
        }
        else {
          rt[0] = temp_fac * rt1[0] + rt2[0];
          rt[1] = temp_fac * rt1[1] + rt2[1];
          rt[2] = temp_fac * rt1[2] + rt2[2];
          rt[3] = temp_fac * rt1[3] + rt2[3];
        }
      }
      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_alphaunder_effect(const SeqRenderData *context,
                                 Sequence * /*seq*/,
                                 float /*timeline_frame*/,
                                 float fac,
                                 ImBuf *ibuf1,
                                 ImBuf *ibuf2,
                                 ImBuf * /*ibuf3*/,
                                 int start_line,
                                 int total_lines,
                                 ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_alphaunder_effect_float(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_alphaunder_effect_byte(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cross Effect
 * \{ */

static void do_cross_effect_byte(float fac, int x, int y, uchar *rect1, uchar *rect2, uchar *out)
{
  uchar *rt1 = rect1;
  uchar *rt2 = rect2;
  uchar *rt = out;

  int temp_fac = int(256.0f * fac);
  int temp_mfac = 256 - temp_fac;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      rt[0] = (temp_mfac * rt1[0] + temp_fac * rt2[0]) >> 8;
      rt[1] = (temp_mfac * rt1[1] + temp_fac * rt2[1]) >> 8;
      rt[2] = (temp_mfac * rt1[2] + temp_fac * rt2[2]) >> 8;
      rt[3] = (temp_mfac * rt1[3] + temp_fac * rt2[3]) >> 8;

      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_cross_effect_float(float fac, int x, int y, float *rect1, float *rect2, float *out)
{
  float *rt1 = rect1;
  float *rt2 = rect2;
  float *rt = out;

  float mfac = 1.0f - fac;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      rt[0] = mfac * rt1[0] + fac * rt2[0];
      rt[1] = mfac * rt1[1] + fac * rt2[1];
      rt[2] = mfac * rt1[2] + fac * rt2[2];
      rt[3] = mfac * rt1[3] + fac * rt2[3];

      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_cross_effect(const SeqRenderData *context,
                            Sequence * /*seq*/,
                            float /*timeline_frame*/,
                            float fac,
                            ImBuf *ibuf1,
                            ImBuf *ibuf2,
                            ImBuf * /*ibuf3*/,
                            int start_line,
                            int total_lines,
                            ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_cross_effect_float(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_cross_effect_byte(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gamma Cross
 * \{ */

/* One could argue that gamma cross should not be hardcoded to 2.0 gamma,
 * but instead either do proper input->linear conversion (often sRGB). Or
 * maybe not even that, but do interpolation in some perceptual color space
 * like Oklab. But currently it is fixed to just 2.0 gamma. */

static float gammaCorrect(float c)
{
  if (UNLIKELY(c < 0)) {
    return -(c * c);
  }
  return c * c;
}

static float invGammaCorrect(float c)
{
  return sqrtf_signed(c);
}

static void do_gammacross_effect_byte(
    float fac, int x, int y, uchar *rect1, uchar *rect2, uchar *out)
{
  uchar *cp1 = rect1;
  uchar *cp2 = rect2;
  uchar *rt = out;

  float mfac = 1.0f - fac;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      float rt1[4], rt2[4], tempc[4];

      straight_uchar_to_premul_float(rt1, cp1);
      straight_uchar_to_premul_float(rt2, cp2);

      tempc[0] = gammaCorrect(mfac * invGammaCorrect(rt1[0]) + fac * invGammaCorrect(rt2[0]));
      tempc[1] = gammaCorrect(mfac * invGammaCorrect(rt1[1]) + fac * invGammaCorrect(rt2[1]));
      tempc[2] = gammaCorrect(mfac * invGammaCorrect(rt1[2]) + fac * invGammaCorrect(rt2[2]));
      tempc[3] = gammaCorrect(mfac * invGammaCorrect(rt1[3]) + fac * invGammaCorrect(rt2[3]));

      premul_float_to_straight_uchar(rt, tempc);
      cp1 += 4;
      cp2 += 4;
      rt += 4;
    }
  }
}

static void do_gammacross_effect_float(
    float fac, int x, int y, float *rect1, float *rect2, float *out)
{
  float *rt1 = rect1;
  float *rt2 = rect2;
  float *rt = out;

  float mfac = 1.0f - fac;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      rt[0] = gammaCorrect(mfac * invGammaCorrect(rt1[0]) + fac * invGammaCorrect(rt2[0]));
      rt[1] = gammaCorrect(mfac * invGammaCorrect(rt1[1]) + fac * invGammaCorrect(rt2[1]));
      rt[2] = gammaCorrect(mfac * invGammaCorrect(rt1[2]) + fac * invGammaCorrect(rt2[2]));
      rt[3] = gammaCorrect(mfac * invGammaCorrect(rt1[3]) + fac * invGammaCorrect(rt2[3]));
      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static ImBuf *gammacross_init_execution(const SeqRenderData *context,
                                        ImBuf *ibuf1,
                                        ImBuf *ibuf2,
                                        ImBuf *ibuf3)
{
  ImBuf *out = prepare_effect_imbufs(context, ibuf1, ibuf2, ibuf3);
  return out;
}

static void do_gammacross_effect(const SeqRenderData *context,
                                 Sequence * /*seq*/,
                                 float /*timeline_frame*/,
                                 float fac,
                                 ImBuf *ibuf1,
                                 ImBuf *ibuf2,
                                 ImBuf * /*ibuf3*/,
                                 int start_line,
                                 int total_lines,
                                 ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_gammacross_effect_float(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_gammacross_effect_byte(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Add Effect
 * \{ */

static void do_add_effect_byte(float fac, int x, int y, uchar *rect1, uchar *rect2, uchar *out)
{
  uchar *cp1 = rect1;
  uchar *cp2 = rect2;
  uchar *rt = out;

  int temp_fac = int(256.0f * fac);

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      const int temp_fac2 = temp_fac * int(cp2[3]);
      rt[0] = min_ii(cp1[0] + ((temp_fac2 * cp2[0]) >> 16), 255);
      rt[1] = min_ii(cp1[1] + ((temp_fac2 * cp2[1]) >> 16), 255);
      rt[2] = min_ii(cp1[2] + ((temp_fac2 * cp2[2]) >> 16), 255);
      rt[3] = cp1[3];

      cp1 += 4;
      cp2 += 4;
      rt += 4;
    }
  }
}

static void do_add_effect_float(float fac, int x, int y, float *rect1, float *rect2, float *out)
{
  float *rt1 = rect1;
  float *rt2 = rect2;
  float *rt = out;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      const float temp_fac = (1.0f - (rt1[3] * (1.0f - fac))) * rt2[3];
      rt[0] = rt1[0] + temp_fac * rt2[0];
      rt[1] = rt1[1] + temp_fac * rt2[1];
      rt[2] = rt1[2] + temp_fac * rt2[2];
      rt[3] = rt1[3];

      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_add_effect(const SeqRenderData *context,
                          Sequence * /*seq*/,
                          float /*timeline_frame*/,
                          float fac,
                          ImBuf *ibuf1,
                          ImBuf *ibuf2,
                          ImBuf * /*ibuf3*/,
                          int start_line,
                          int total_lines,
                          ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_add_effect_float(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_add_effect_byte(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Subtract Effect
 * \{ */

static void do_sub_effect_byte(float fac, int x, int y, uchar *rect1, uchar *rect2, uchar *out)
{
  uchar *cp1 = rect1;
  uchar *cp2 = rect2;
  uchar *rt = out;

  int temp_fac = int(256.0f * fac);

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      const int temp_fac2 = temp_fac * int(cp2[3]);
      rt[0] = max_ii(cp1[0] - ((temp_fac2 * cp2[0]) >> 16), 0);
      rt[1] = max_ii(cp1[1] - ((temp_fac2 * cp2[1]) >> 16), 0);
      rt[2] = max_ii(cp1[2] - ((temp_fac2 * cp2[2]) >> 16), 0);
      rt[3] = cp1[3];

      cp1 += 4;
      cp2 += 4;
      rt += 4;
    }
  }
}

static void do_sub_effect_float(float fac, int x, int y, float *rect1, float *rect2, float *out)
{
  float *rt1 = rect1;
  float *rt2 = rect2;
  float *rt = out;

  float mfac = 1.0f - fac;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      const float temp_fac = (1.0f - (rt1[3] * mfac)) * rt2[3];
      rt[0] = max_ff(rt1[0] - temp_fac * rt2[0], 0.0f);
      rt[1] = max_ff(rt1[1] - temp_fac * rt2[1], 0.0f);
      rt[2] = max_ff(rt1[2] - temp_fac * rt2[2], 0.0f);
      rt[3] = rt1[3];

      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_sub_effect(const SeqRenderData *context,
                          Sequence * /*seq*/,
                          float /*timeline_frame*/,
                          float fac,
                          ImBuf *ibuf1,
                          ImBuf *ibuf2,
                          ImBuf * /*ibuf3*/,
                          int start_line,
                          int total_lines,
                          ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_sub_effect_float(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_sub_effect_byte(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drop Effect
 * \{ */

/* Must be > 0 or add pre-copy, etc to the function. */
#define XOFF 8
#define YOFF 8

static void do_drop_effect_byte(float fac, int x, int y, uchar *rect2i, uchar *rect1i, uchar *outi)
{
  const int xoff = min_ii(XOFF, x);
  const int yoff = min_ii(YOFF, y);

  int temp_fac = int(70.0f * fac);

  uchar *rt2 = rect2i + yoff * 4 * x;
  uchar *rt1 = rect1i;
  uchar *out = outi;
  for (int i = 0; i < y - yoff; i++) {
    memcpy(out, rt1, sizeof(*out) * xoff * 4);
    rt1 += xoff * 4;
    out += xoff * 4;

    for (int j = xoff; j < x; j++) {
      int temp_fac2 = ((temp_fac * rt2[3]) >> 8);

      *(out++) = std::max(0, *rt1 - temp_fac2);
      rt1++;
      *(out++) = std::max(0, *rt1 - temp_fac2);
      rt1++;
      *(out++) = std::max(0, *rt1 - temp_fac2);
      rt1++;
      *(out++) = std::max(0, *rt1 - temp_fac2);
      rt1++;
      rt2 += 4;
    }
    rt2 += xoff * 4;
  }
  memcpy(out, rt1, sizeof(*out) * yoff * 4 * x);
}

static void do_drop_effect_float(
    float fac, int x, int y, float *rect2i, float *rect1i, float *outi)
{
  const int xoff = min_ii(XOFF, x);
  const int yoff = min_ii(YOFF, y);

  float temp_fac = 70.0f * fac;

  float *rt2 = rect2i + yoff * 4 * x;
  float *rt1 = rect1i;
  float *out = outi;
  for (int i = 0; i < y - yoff; i++) {
    memcpy(out, rt1, sizeof(*out) * xoff * 4);
    rt1 += xoff * 4;
    out += xoff * 4;

    for (int j = xoff; j < x; j++) {
      float temp_fac2 = temp_fac * rt2[3];

      *(out++) = std::max(0.0f, *rt1 - temp_fac2);
      rt1++;
      *(out++) = std::max(0.0f, *rt1 - temp_fac2);
      rt1++;
      *(out++) = std::max(0.0f, *rt1 - temp_fac2);
      rt1++;
      *(out++) = std::max(0.0f, *rt1 - temp_fac2);
      rt1++;
      rt2 += 4;
    }
    rt2 += xoff * 4;
  }
  memcpy(out, rt1, sizeof(*out) * yoff * 4 * x);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Multiply Effect
 * \{ */

static void do_mul_effect_byte(float fac, int x, int y, uchar *rect1, uchar *rect2, uchar *out)
{
  uchar *rt1 = rect1;
  uchar *rt2 = rect2;
  uchar *rt = out;

  int temp_fac = int(256.0f * fac);

  /* Formula:
   * `fac * (a * b) + (1 - fac) * a => fac * a * (b - 1) + axaux = c * px + py * s;` // + centx
   * `yaux = -s * px + c * py;` // + centy */

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      rt[0] = rt1[0] + ((temp_fac * rt1[0] * (rt2[0] - 255)) >> 16);
      rt[1] = rt1[1] + ((temp_fac * rt1[1] * (rt2[1] - 255)) >> 16);
      rt[2] = rt1[2] + ((temp_fac * rt1[2] * (rt2[2] - 255)) >> 16);
      rt[3] = rt1[3] + ((temp_fac * rt1[3] * (rt2[3] - 255)) >> 16);

      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_mul_effect_float(float fac, int x, int y, float *rect1, float *rect2, float *out)
{
  float *rt1 = rect1;
  float *rt2 = rect2;
  float *rt = out;

  /* Formula:
   * `fac * (a * b) + (1 - fac) * a => fac * a * (b - 1) + a`. */

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      rt[0] = rt1[0] + fac * rt1[0] * (rt2[0] - 1.0f);
      rt[1] = rt1[1] + fac * rt1[1] * (rt2[1] - 1.0f);
      rt[2] = rt1[2] + fac * rt1[2] * (rt2[2] - 1.0f);
      rt[3] = rt1[3] + fac * rt1[3] * (rt2[3] - 1.0f);

      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_mul_effect(const SeqRenderData *context,
                          Sequence * /*seq*/,
                          float /*timeline_frame*/,
                          float fac,
                          ImBuf *ibuf1,
                          ImBuf *ibuf2,
                          ImBuf * /*ibuf3*/,
                          int start_line,
                          int total_lines,
                          ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_mul_effect_float(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_mul_effect_byte(fac, context->rectx, total_lines, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Blend Mode Effect
 * \{ */

using IMB_blend_func_byte = void (*)(uchar *dst, const uchar *src1, const uchar *src2);
using IMB_blend_func_float = void (*)(float *dst, const float *src1, const float *src2);

BLI_INLINE void apply_blend_function_byte(float fac,
                                          int x,
                                          int y,
                                          uchar *rect1,
                                          uchar *rect2,
                                          uchar *out,
                                          IMB_blend_func_byte blend_function)
{
  uchar *rt1 = rect1;
  uchar *rt2 = rect2;
  uchar *rt = out;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      uint achannel = rt2[3];
      rt2[3] = uint(achannel) * fac;
      blend_function(rt, rt1, rt2);
      rt2[3] = achannel;
      rt[3] = rt1[3];
      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

BLI_INLINE void apply_blend_function_float(float fac,
                                           int x,
                                           int y,
                                           float *rect1,
                                           float *rect2,
                                           float *out,
                                           IMB_blend_func_float blend_function)
{
  float *rt1 = rect1;
  float *rt2 = rect2;
  float *rt = out;

  for (int i = 0; i < y; i++) {
    for (int j = 0; j < x; j++) {
      float achannel = rt2[3];
      rt2[3] = achannel * fac;
      blend_function(rt, rt1, rt2);
      rt2[3] = achannel;
      rt[3] = rt1[3];
      rt1 += 4;
      rt2 += 4;
      rt += 4;
    }
  }
}

static void do_blend_effect_float(
    float fac, int x, int y, float *rect1, float *rect2, int btype, float *out)
{
  switch (btype) {
    case SEQ_TYPE_ADD:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_add_float);
      break;
    case SEQ_TYPE_SUB:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_sub_float);
      break;
    case SEQ_TYPE_MUL:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_mul_float);
      break;
    case SEQ_TYPE_DARKEN:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_darken_float);
      break;
    case SEQ_TYPE_COLOR_BURN:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_burn_float);
      break;
    case SEQ_TYPE_LINEAR_BURN:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_linearburn_float);
      break;
    case SEQ_TYPE_SCREEN:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_screen_float);
      break;
    case SEQ_TYPE_LIGHTEN:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_lighten_float);
      break;
    case SEQ_TYPE_DODGE:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_dodge_float);
      break;
    case SEQ_TYPE_OVERLAY:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_overlay_float);
      break;
    case SEQ_TYPE_SOFT_LIGHT:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_softlight_float);
      break;
    case SEQ_TYPE_HARD_LIGHT:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_hardlight_float);
      break;
    case SEQ_TYPE_PIN_LIGHT:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_pinlight_float);
      break;
    case SEQ_TYPE_LIN_LIGHT:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_linearlight_float);
      break;
    case SEQ_TYPE_VIVID_LIGHT:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_vividlight_float);
      break;
    case SEQ_TYPE_BLEND_COLOR:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_color_float);
      break;
    case SEQ_TYPE_HUE:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_hue_float);
      break;
    case SEQ_TYPE_SATURATION:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_saturation_float);
      break;
    case SEQ_TYPE_VALUE:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_luminosity_float);
      break;
    case SEQ_TYPE_DIFFERENCE:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_difference_float);
      break;
    case SEQ_TYPE_EXCLUSION:
      apply_blend_function_float(fac, x, y, rect1, rect2, out, blend_color_exclusion_float);
      break;
    default:
      break;
  }
}

static void do_blend_effect_byte(
    float fac, int x, int y, uchar *rect1, uchar *rect2, int btype, uchar *out)
{
  switch (btype) {
    case SEQ_TYPE_ADD:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_add_byte);
      break;
    case SEQ_TYPE_SUB:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_sub_byte);
      break;
    case SEQ_TYPE_MUL:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_mul_byte);
      break;
    case SEQ_TYPE_DARKEN:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_darken_byte);
      break;
    case SEQ_TYPE_COLOR_BURN:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_burn_byte);
      break;
    case SEQ_TYPE_LINEAR_BURN:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_linearburn_byte);
      break;
    case SEQ_TYPE_SCREEN:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_screen_byte);
      break;
    case SEQ_TYPE_LIGHTEN:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_lighten_byte);
      break;
    case SEQ_TYPE_DODGE:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_dodge_byte);
      break;
    case SEQ_TYPE_OVERLAY:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_overlay_byte);
      break;
    case SEQ_TYPE_SOFT_LIGHT:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_softlight_byte);
      break;
    case SEQ_TYPE_HARD_LIGHT:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_hardlight_byte);
      break;
    case SEQ_TYPE_PIN_LIGHT:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_pinlight_byte);
      break;
    case SEQ_TYPE_LIN_LIGHT:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_linearlight_byte);
      break;
    case SEQ_TYPE_VIVID_LIGHT:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_vividlight_byte);
      break;
    case SEQ_TYPE_BLEND_COLOR:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_color_byte);
      break;
    case SEQ_TYPE_HUE:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_hue_byte);
      break;
    case SEQ_TYPE_SATURATION:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_saturation_byte);
      break;
    case SEQ_TYPE_VALUE:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_luminosity_byte);
      break;
    case SEQ_TYPE_DIFFERENCE:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_difference_byte);
      break;
    case SEQ_TYPE_EXCLUSION:
      apply_blend_function_byte(fac, x, y, rect1, rect2, out, blend_color_exclusion_byte);
      break;
    default:
      break;
  }
}

static void do_blend_mode_effect(const SeqRenderData *context,
                                 Sequence *seq,
                                 float /*timeline_frame*/,
                                 float fac,
                                 ImBuf *ibuf1,
                                 ImBuf *ibuf2,
                                 ImBuf * /*ibuf3*/,
                                 int start_line,
                                 int total_lines,
                                 ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;
    slice_get_float_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);
    do_blend_effect_float(
        fac, context->rectx, total_lines, rect1, rect2, seq->blend_mode, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;
    slice_get_byte_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);
    do_blend_effect_byte(
        fac, context->rectx, total_lines, rect1, rect2, seq->blend_mode, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Mix Effect
 * \{ */

static void init_colormix_effect(Sequence *seq)
{
  ColorMixVars *data;

  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }
  seq->effectdata = MEM_callocN(sizeof(ColorMixVars), "colormixvars");
  data = (ColorMixVars *)seq->effectdata;
  data->blend_effect = SEQ_TYPE_OVERLAY;
  data->factor = 1.0f;
}

static void do_colormix_effect(const SeqRenderData *context,
                               Sequence *seq,
                               float /*timeline_frame*/,
                               float /*fac*/,
                               ImBuf *ibuf1,
                               ImBuf *ibuf2,
                               ImBuf * /*ibuf3*/,
                               int start_line,
                               int total_lines,
                               ImBuf *out)
{
  float fac;

  ColorMixVars *data = static_cast<ColorMixVars *>(seq->effectdata);
  fac = data->factor;

  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;
    slice_get_float_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);
    do_blend_effect_float(
        fac, context->rectx, total_lines, rect1, rect2, data->blend_effect, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;
    slice_get_byte_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);
    do_blend_effect_byte(
        fac, context->rectx, total_lines, rect1, rect2, data->blend_effect, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Wipe Effect
 * \{ */

struct WipeZone {
  float angle;
  int flip;
  int xo, yo;
  int width;
  float pythangle;
  float clockWidth;
  int type;
  bool forward;
};

static WipeZone precalc_wipe_zone(const WipeVars *wipe, int xo, int yo)
{
  WipeZone zone;
  zone.flip = (wipe->angle < 0.0f);
  zone.angle = tanf(fabsf(wipe->angle));
  zone.xo = xo;
  zone.yo = yo;
  zone.width = int(wipe->edgeWidth * ((xo + yo) / 2.0f));
  zone.pythangle = 1.0f / sqrtf(zone.angle * zone.angle + 1.0f);
  zone.clockWidth = wipe->edgeWidth * float(M_PI);
  zone.type = wipe->wipetype;
  zone.forward = wipe->forward != 0;
  return zone;
}

/**
 * This function calculates the blur band for the wipe effects.
 */
static float in_band(float width, float dist, int side, int dir)
{
  float alpha;

  if (width == 0) {
    return float(side);
  }

  if (width < dist) {
    return float(side);
  }

  if (side == 1) {
    alpha = (dist + 0.5f * width) / (width);
  }
  else {
    alpha = (0.5f * width - dist) / (width);
  }

  if (dir == 0) {
    alpha = 1 - alpha;
  }

  return alpha;
}

static float check_zone(const WipeZone *wipezone, int x, int y, float fac)
{
  float posx, posy, hyp, hyp2, angle, hwidth, b1, b2, b3, pointdist;
  float temp1, temp2, temp3, temp4; /* some placeholder variables */
  int xo = wipezone->xo;
  int yo = wipezone->yo;
  float halfx = xo * 0.5f;
  float halfy = yo * 0.5f;
  float widthf, output = 0;
  int width;

  if (wipezone->flip) {
    x = xo - x;
  }
  angle = wipezone->angle;

  if (wipezone->forward) {
    posx = fac * xo;
    posy = fac * yo;
  }
  else {
    posx = xo - fac * xo;
    posy = yo - fac * yo;
  }

  switch (wipezone->type) {
    case DO_SINGLE_WIPE:
      width = min_ii(wipezone->width, fac * yo);
      width = min_ii(width, yo - fac * yo);

      if (angle == 0.0f) {
        b1 = posy;
        b2 = y;
        hyp = fabsf(y - posy);
      }
      else {
        b1 = posy - (-angle) * posx;
        b2 = y - (-angle) * x;
        hyp = fabsf(angle * x + y + (-posy - angle * posx)) * wipezone->pythangle;
      }

      if (angle < 0) {
        temp1 = b1;
        b1 = b2;
        b2 = temp1;
      }

      if (wipezone->forward) {
        if (b1 < b2) {
          output = in_band(width, hyp, 1, 1);
        }
        else {
          output = in_band(width, hyp, 0, 1);
        }
      }
      else {
        if (b1 < b2) {
          output = in_band(width, hyp, 0, 1);
        }
        else {
          output = in_band(width, hyp, 1, 1);
        }
      }
      break;

    case DO_DOUBLE_WIPE:
      if (!wipezone->forward) {
        fac = 1.0f - fac; /* Go the other direction */
      }

      width = wipezone->width; /* calculate the blur width */
      hwidth = width * 0.5f;
      if (angle == 0) {
        b1 = posy * 0.5f;
        b3 = yo - posy * 0.5f;
        b2 = y;

        hyp = fabsf(y - posy * 0.5f);
        hyp2 = fabsf(y - (yo - posy * 0.5f));
      }
      else {
        b1 = posy * 0.5f - (-angle) * posx * 0.5f;
        b3 = (yo - posy * 0.5f) - (-angle) * (xo - posx * 0.5f);
        b2 = y - (-angle) * x;

        hyp = fabsf(angle * x + y + (-posy * 0.5f - angle * posx * 0.5f)) * wipezone->pythangle;
        hyp2 = fabsf(angle * x + y + (-(yo - posy * 0.5f) - angle * (xo - posx * 0.5f))) *
               wipezone->pythangle;
      }

      hwidth = min_ff(hwidth, fabsf(b3 - b1) / 2.0f);

      if (b2 < b1 && b2 < b3) {
        output = in_band(hwidth, hyp, 0, 1);
      }
      else if (b2 > b1 && b2 > b3) {
        output = in_band(hwidth, hyp2, 0, 1);
      }
      else {
        if (hyp < hwidth && hyp2 > hwidth) {
          output = in_band(hwidth, hyp, 1, 1);
        }
        else if (hyp > hwidth && hyp2 < hwidth) {
          output = in_band(hwidth, hyp2, 1, 1);
        }
        else {
          output = in_band(hwidth, hyp2, 1, 1) * in_band(hwidth, hyp, 1, 1);
        }
      }
      if (!wipezone->forward) {
        output = 1 - output;
      }
      break;
    case DO_CLOCK_WIPE:
      /*
       * temp1: angle of effect center in rads
       * temp2: angle of line through (halfx, halfy) and (x, y) in rads
       * temp3: angle of low side of blur
       * temp4: angle of high side of blur
       */
      output = 1.0f - fac;
      widthf = wipezone->clockWidth;
      temp1 = 2.0f * float(M_PI) * fac;

      if (wipezone->forward) {
        temp1 = 2.0f * float(M_PI) - temp1;
      }

      x = x - halfx;
      y = y - halfy;

      temp2 = atan2f(y, x);
      if (temp2 < 0.0f) {
        temp2 += 2.0f * float(M_PI);
      }

      if (wipezone->forward) {
        temp3 = temp1 - widthf * fac;
        temp4 = temp1 + widthf * (1 - fac);
      }
      else {
        temp3 = temp1 - widthf * (1 - fac);
        temp4 = temp1 + widthf * fac;
      }
      if (temp3 < 0) {
        temp3 = 0;
      }
      if (temp4 > 2.0f * float(M_PI)) {
        temp4 = 2.0f * float(M_PI);
      }

      if (temp2 < temp3) {
        output = 0;
      }
      else if (temp2 > temp4) {
        output = 1;
      }
      else {
        output = (temp2 - temp3) / (temp4 - temp3);
      }
      if (x == 0 && y == 0) {
        output = 1;
      }
      if (output != output) {
        output = 1;
      }
      if (wipezone->forward) {
        output = 1 - output;
      }
      break;
    case DO_IRIS_WIPE:
      if (xo > yo) {
        yo = xo;
      }
      else {
        xo = yo;
      }

      if (!wipezone->forward) {
        fac = 1 - fac;
      }

      width = wipezone->width;
      hwidth = width * 0.5f;

      temp1 = (halfx - (halfx)*fac);
      pointdist = hypotf(temp1, temp1);

      temp2 = hypotf(halfx - x, halfy - y);
      if (temp2 > pointdist) {
        output = in_band(hwidth, fabsf(temp2 - pointdist), 0, 1);
      }
      else {
        output = in_band(hwidth, fabsf(temp2 - pointdist), 1, 1);
      }

      if (!wipezone->forward) {
        output = 1 - output;
      }

      break;
  }
  if (output < 0) {
    output = 0;
  }
  else if (output > 1) {
    output = 1;
  }
  return output;
}

static void init_wipe_effect(Sequence *seq)
{
  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  seq->effectdata = MEM_callocN(sizeof(WipeVars), "wipevars");
}

static int num_inputs_wipe()
{
  return 2;
}

static void free_wipe_effect(Sequence *seq, const bool /*do_id_user*/)
{
  MEM_SAFE_FREE(seq->effectdata);
}

static void copy_wipe_effect(Sequence *dst, Sequence *src, const int /*flag*/)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
}

static void do_wipe_effect_byte(const Sequence *seq,
                                float fac,
                                int width,
                                int height,
                                const uchar *rect1,
                                const uchar *rect2,
                                uchar *out)
{
  using namespace blender;
  const WipeVars *wipe = (const WipeVars *)seq->effectdata;
  const WipeZone wipezone = precalc_wipe_zone(wipe, width, height);

  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange y_range) {
    const uchar *cp1 = rect1 + y_range.first() * width * 4;
    const uchar *cp2 = rect2 + y_range.first() * width * 4;
    uchar *rt = out + y_range.first() * width * 4;
    for (const int y : y_range) {
      for (int x = 0; x < width; x++) {
        float check = check_zone(&wipezone, x, y, fac);
        if (check) {
          if (cp1) {
            float rt1[4], rt2[4], tempc[4];

            straight_uchar_to_premul_float(rt1, cp1);
            straight_uchar_to_premul_float(rt2, cp2);

            tempc[0] = rt1[0] * check + rt2[0] * (1 - check);
            tempc[1] = rt1[1] * check + rt2[1] * (1 - check);
            tempc[2] = rt1[2] * check + rt2[2] * (1 - check);
            tempc[3] = rt1[3] * check + rt2[3] * (1 - check);

            premul_float_to_straight_uchar(rt, tempc);
          }
          else {
            rt[0] = 0;
            rt[1] = 0;
            rt[2] = 0;
            rt[3] = 255;
          }
        }
        else {
          if (cp2) {
            rt[0] = cp2[0];
            rt[1] = cp2[1];
            rt[2] = cp2[2];
            rt[3] = cp2[3];
          }
          else {
            rt[0] = 0;
            rt[1] = 0;
            rt[2] = 0;
            rt[3] = 255;
          }
        }

        rt += 4;
        if (cp1 != nullptr) {
          cp1 += 4;
        }
        if (cp2 != nullptr) {
          cp2 += 4;
        }
      }
    }
  });
}

static void do_wipe_effect_float(Sequence *seq,
                                 float fac,
                                 int width,
                                 int height,
                                 const float *rect1,
                                 const float *rect2,
                                 float *out)
{
  using namespace blender;
  const WipeVars *wipe = (const WipeVars *)seq->effectdata;
  const WipeZone wipezone = precalc_wipe_zone(wipe, width, height);

  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange y_range) {
    const float *rt1 = rect1 + y_range.first() * width * 4;
    const float *rt2 = rect2 + y_range.first() * width * 4;
    float *rt = out + y_range.first() * width * 4;
    for (const int y : y_range) {
      for (int x = 0; x < width; x++) {
        float check = check_zone(&wipezone, x, y, fac);
        if (check) {
          if (rt1) {
            rt[0] = rt1[0] * check + rt2[0] * (1 - check);
            rt[1] = rt1[1] * check + rt2[1] * (1 - check);
            rt[2] = rt1[2] * check + rt2[2] * (1 - check);
            rt[3] = rt1[3] * check + rt2[3] * (1 - check);
          }
          else {
            rt[0] = 0;
            rt[1] = 0;
            rt[2] = 0;
            rt[3] = 1.0;
          }
        }
        else {
          if (rt2) {
            rt[0] = rt2[0];
            rt[1] = rt2[1];
            rt[2] = rt2[2];
            rt[3] = rt2[3];
          }
          else {
            rt[0] = 0;
            rt[1] = 0;
            rt[2] = 0;
            rt[3] = 1.0;
          }
        }

        rt += 4;
        if (rt1 != nullptr) {
          rt1 += 4;
        }
        if (rt2 != nullptr) {
          rt2 += 4;
        }
      }
    }
  });
}

static ImBuf *do_wipe_effect(const SeqRenderData *context,
                             Sequence *seq,
                             float /*timeline_frame*/,
                             float fac,
                             ImBuf *ibuf1,
                             ImBuf *ibuf2,
                             ImBuf *ibuf3)
{
  ImBuf *out = prepare_effect_imbufs(context, ibuf1, ibuf2, ibuf3);

  if (out->float_buffer.data) {
    do_wipe_effect_float(seq,
                         fac,
                         context->rectx,
                         context->recty,
                         ibuf1->float_buffer.data,
                         ibuf2->float_buffer.data,
                         out->float_buffer.data);
  }
  else {
    do_wipe_effect_byte(seq,
                        fac,
                        context->rectx,
                        context->recty,
                        ibuf1->byte_buffer.data,
                        ibuf2->byte_buffer.data,
                        out->byte_buffer.data);
  }

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform Effect
 * \{ */

static void init_transform_effect(Sequence *seq)
{
  TransformVars *transform;

  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  seq->effectdata = MEM_callocN(sizeof(TransformVars), "transformvars");

  transform = (TransformVars *)seq->effectdata;

  transform->ScalexIni = 1.0f;
  transform->ScaleyIni = 1.0f;

  transform->xIni = 0.0f;
  transform->yIni = 0.0f;

  transform->rotIni = 0.0f;

  transform->interpolation = 1;
  transform->percent = 1;
  transform->uniform_scale = 0;
}

static int num_inputs_transform()
{
  return 1;
}

static void free_transform_effect(Sequence *seq, const bool /*do_id_user*/)
{
  MEM_SAFE_FREE(seq->effectdata);
}

static void copy_transform_effect(Sequence *dst, Sequence *src, const int /*flag*/)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
}

static void transform_image(int x,
                            int y,
                            int start_line,
                            int total_lines,
                            ImBuf *ibuf1,
                            ImBuf *out,
                            float scale_x,
                            float scale_y,
                            float translate_x,
                            float translate_y,
                            float rotate,
                            int interpolation)
{
  /* Rotate */
  float s = sinf(rotate);
  float c = cosf(rotate);

  for (int yi = start_line; yi < start_line + total_lines; yi++) {
    for (int xi = 0; xi < x; xi++) {
      /* Translate point. */
      float xt = xi - translate_x;
      float yt = yi - translate_y;

      /* Rotate point with center ref. */
      float xr = c * xt + s * yt;
      float yr = -s * xt + c * yt;

      /* Scale point with center ref. */
      xt = xr / scale_x;
      yt = yr / scale_y;

      /* Undo reference center point. */
      xt += (x / 2.0f);
      yt += (y / 2.0f);

      /* interpolate */
      switch (interpolation) {
        case 0:
          nearest_interpolation(ibuf1, out, xt, yt, xi, yi);
          break;
        case 1:
          bilinear_interpolation(ibuf1, out, xt, yt, xi, yi);
          break;
        case 2:
          bicubic_interpolation(ibuf1, out, xt, yt, xi, yi);
          break;
      }
    }
  }
}

static void do_transform_effect(const SeqRenderData *context,
                                Sequence *seq,
                                float /*timeline_frame*/,
                                float /*fac*/,
                                ImBuf *ibuf1,
                                ImBuf * /*ibuf2*/,
                                ImBuf * /*ibuf3*/,
                                int start_line,
                                int total_lines,
                                ImBuf *out)
{
  TransformVars *transform = (TransformVars *)seq->effectdata;
  float scale_x, scale_y, translate_x, translate_y, rotate_radians;

  /* Scale */
  if (transform->uniform_scale) {
    scale_x = scale_y = transform->ScalexIni;
  }
  else {
    scale_x = transform->ScalexIni;
    scale_y = transform->ScaleyIni;
  }

  int x = context->rectx;
  int y = context->recty;

  /* Translate */
  if (!transform->percent) {
    /* Compensate text size for preview render size. */
    double proxy_size_comp = context->scene->r.size / 100.0;
    if (context->preview_render_size != SEQ_RENDER_SIZE_SCENE) {
      proxy_size_comp = SEQ_rendersize_to_scale_factor(context->preview_render_size);
    }

    translate_x = transform->xIni * proxy_size_comp + (x / 2.0f);
    translate_y = transform->yIni * proxy_size_comp + (y / 2.0f);
  }
  else {
    translate_x = x * (transform->xIni / 100.0f) + (x / 2.0f);
    translate_y = y * (transform->yIni / 100.0f) + (y / 2.0f);
  }

  /* Rotate */
  rotate_radians = DEG2RADF(transform->rotIni);

  transform_image(x,
                  y,
                  start_line,
                  total_lines,
                  ibuf1,
                  out,
                  scale_x,
                  scale_y,
                  translate_x,
                  translate_y,
                  rotate_radians,
                  transform->interpolation);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Glow Effect
 * \{ */

static void glow_blur_bitmap(const blender::float4 *src,
                             blender::float4 *map,
                             int width,
                             int height,
                             float blur,
                             int quality)
{
  using namespace blender;

  /* If we're not really blurring, bail out */
  if (blur <= 0) {
    return;
  }

  /* If result would be no blurring, early out. */
  const int halfWidth = ((quality + 1) * blur);
  if (halfWidth == 0) {
    return;
  }

  Array<float4> temp(width * height);

  /* Initialize the gaussian filter. @TODO: use code from RE_filter_value */
  Array<float> filter(halfWidth * 2);
  const float k = -1.0f / (2.0f * float(M_PI) * blur * blur);
  float weight = 0;
  for (int ix = 0; ix < halfWidth; ix++) {
    weight = float(exp(k * (ix * ix)));
    filter[halfWidth - ix] = weight;
    filter[halfWidth + ix] = weight;
  }
  filter[0] = weight;
  /* Normalize the array */
  float fval = 0;
  for (int ix = 0; ix < halfWidth * 2; ix++) {
    fval += filter[ix];
  }
  for (int ix = 0; ix < halfWidth * 2; ix++) {
    filter[ix] /= fval;
  }

  /* Blur the rows: read map, write temp */
  threading::parallel_for(IndexRange(height), 32, [&](const IndexRange y_range) {
    for (const int y : y_range) {
      for (int x = 0; x < width; x++) {
        float4 curColor = float4(0.0f);
        int xmin = math::max(x - halfWidth, 0);
        int xmax = math::min(x + halfWidth, width);
        for (int nx = xmin, index = (xmin - x) + halfWidth; nx < xmax; nx++, index++) {
          curColor += map[nx + y * width] * filter[index];
        }
        temp[x + y * width] = curColor;
      }
    }
  });

  /* Blur the columns: read temp, write map */
  threading::parallel_for(IndexRange(width), 32, [&](const IndexRange x_range) {
    const float4 one = float4(1.0f);
    for (const int x : x_range) {
      for (int y = 0; y < height; y++) {
        float4 curColor = float4(0.0f);
        int ymin = math::max(y - halfWidth, 0);
        int ymax = math::min(y + halfWidth, height);
        for (int ny = ymin, index = (ymin - y) + halfWidth; ny < ymax; ny++, index++) {
          curColor += temp[x + ny * width] * filter[index];
        }
        if (src != nullptr) {
          curColor = math::min(one, src[x + y * width] + curColor);
        }
        map[x + y * width] = curColor;
      }
    }
  });
}

static void blur_isolate_highlights(const blender::float4 *in,
                                    blender::float4 *out,
                                    int width,
                                    int height,
                                    float threshold,
                                    float boost,
                                    float clamp)
{
  using namespace blender;
  threading::parallel_for(IndexRange(height), 64, [&](const IndexRange y_range) {
    const float4 clampv = float4(clamp);
    for (const int y : y_range) {
      int index = y * width;
      for (int x = 0; x < width; x++, index++) {

        /* Isolate the intensity */
        float intensity = (in[index].x + in[index].y + in[index].z - threshold);
        float4 val;
        if (intensity > 0) {
          val = math::min(clampv, in[index] * (boost * intensity));
        }
        else {
          val = float4(0.0f);
        }
        out[index] = val;
      }
    }
  });
}

static void init_glow_effect(Sequence *seq)
{
  GlowVars *glow;

  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  seq->effectdata = MEM_callocN(sizeof(GlowVars), "glowvars");

  glow = (GlowVars *)seq->effectdata;
  glow->fMini = 0.25;
  glow->fClamp = 1.0;
  glow->fBoost = 0.5;
  glow->dDist = 3.0;
  glow->dQuality = 3;
  glow->bNoComp = 0;
}

static int num_inputs_glow()
{
  return 1;
}

static void free_glow_effect(Sequence *seq, const bool /*do_id_user*/)
{
  MEM_SAFE_FREE(seq->effectdata);
}

static void copy_glow_effect(Sequence *dst, Sequence *src, const int /*flag*/)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
}

static void do_glow_effect_byte(Sequence *seq,
                                int render_size,
                                float fac,
                                int x,
                                int y,
                                uchar *rect1,
                                uchar * /*rect2*/,
                                uchar *out)
{
  using namespace blender;
  GlowVars *glow = (GlowVars *)seq->effectdata;

  Array<float4> inbuf(x * y);
  Array<float4> outbuf(x * y);

  using namespace blender;
  IMB_colormanagement_transform_from_byte_threaded(*inbuf.data(), rect1, x, y, 4, "sRGB", "sRGB");

  blur_isolate_highlights(
      inbuf.data(), outbuf.data(), x, y, glow->fMini * 3.0f, glow->fBoost * fac, glow->fClamp);
  glow_blur_bitmap(glow->bNoComp ? nullptr : inbuf.data(),
                   outbuf.data(),
                   x,
                   y,
                   glow->dDist * (render_size / 100.0f),
                   glow->dQuality);

  threading::parallel_for(IndexRange(y), 64, [&](const IndexRange y_range) {
    size_t offset = y_range.first() * x;
    IMB_buffer_byte_from_float(out + offset * 4,
                               *(outbuf.data() + offset),
                               4,
                               0.0f,
                               IB_PROFILE_SRGB,
                               IB_PROFILE_SRGB,
                               true,
                               x,
                               y_range.size(),
                               x,
                               x);
  });
}

static void do_glow_effect_float(Sequence *seq,
                                 int render_size,
                                 float fac,
                                 int x,
                                 int y,
                                 float *rect1,
                                 float * /*rect2*/,
                                 float *out)
{
  using namespace blender;
  float4 *outbuf = reinterpret_cast<float4 *>(out);
  float4 *inbuf = reinterpret_cast<float4 *>(rect1);
  GlowVars *glow = (GlowVars *)seq->effectdata;

  blur_isolate_highlights(
      inbuf, outbuf, x, y, glow->fMini * 3.0f, glow->fBoost * fac, glow->fClamp);
  glow_blur_bitmap(glow->bNoComp ? nullptr : inbuf,
                   outbuf,
                   x,
                   y,
                   glow->dDist * (render_size / 100.0f),
                   glow->dQuality);
}

static ImBuf *do_glow_effect(const SeqRenderData *context,
                             Sequence *seq,
                             float /*timeline_frame*/,
                             float fac,
                             ImBuf *ibuf1,
                             ImBuf *ibuf2,
                             ImBuf *ibuf3)
{
  ImBuf *out = prepare_effect_imbufs(context, ibuf1, ibuf2, ibuf3);

  int render_size = 100 * context->rectx / context->scene->r.xsch;

  if (out->float_buffer.data) {
    do_glow_effect_float(seq,
                         render_size,
                         fac,
                         context->rectx,
                         context->recty,
                         ibuf1->float_buffer.data,
                         nullptr,
                         out->float_buffer.data);
  }
  else {
    do_glow_effect_byte(seq,
                        render_size,
                        fac,
                        context->rectx,
                        context->recty,
                        ibuf1->byte_buffer.data,
                        nullptr,
                        out->byte_buffer.data);
  }

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Solid Color Effect
 * \{ */

static void init_solid_color(Sequence *seq)
{
  SolidColorVars *cv;

  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  seq->effectdata = MEM_callocN(sizeof(SolidColorVars), "solidcolor");

  cv = (SolidColorVars *)seq->effectdata;
  cv->col[0] = cv->col[1] = cv->col[2] = 0.5;
}

static int num_inputs_color()
{
  return 0;
}

static void free_solid_color(Sequence *seq, const bool /*do_id_user*/)
{
  MEM_SAFE_FREE(seq->effectdata);
}

static void copy_solid_color(Sequence *dst, Sequence *src, const int /*flag*/)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
}

static int early_out_color(Sequence * /*seq*/, float /*fac*/)
{
  return EARLY_NO_INPUT;
}

static ImBuf *do_solid_color(const SeqRenderData *context,
                             Sequence *seq,
                             float /*timeline_frame*/,
                             float /*fac*/,
                             ImBuf *ibuf1,
                             ImBuf *ibuf2,
                             ImBuf *ibuf3)
{
  ImBuf *out = prepare_effect_imbufs(context, ibuf1, ibuf2, ibuf3);

  SolidColorVars *cv = (SolidColorVars *)seq->effectdata;

  int x = out->x;
  int y = out->y;

  if (out->byte_buffer.data) {
    uchar color[4];
    color[0] = cv->col[0] * 255;
    color[1] = cv->col[1] * 255;
    color[2] = cv->col[2] * 255;
    color[3] = 255;

    uchar *rect = out->byte_buffer.data;

    for (int i = 0; i < y; i++) {
      for (int j = 0; j < x; j++) {
        rect[0] = color[0];
        rect[1] = color[1];
        rect[2] = color[2];
        rect[3] = color[3];
        rect += 4;
      }
    }
  }
  else if (out->float_buffer.data) {
    float color[4];
    color[0] = cv->col[0];
    color[1] = cv->col[1];
    color[2] = cv->col[2];
    color[3] = 255;

    float *rect_float = out->float_buffer.data;

    for (int i = 0; i < y; i++) {
      for (int j = 0; j < x; j++) {
        rect_float[0] = color[0];
        rect_float[1] = color[1];
        rect_float[2] = color[2];
        rect_float[3] = color[3];
        rect_float += 4;
      }
    }
  }

  out->planes = R_IMF_PLANES_RGB;

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Multi-Camera Effect
 * \{ */

/** No effect inputs for multi-camera, we use #give_ibuf_seq. */
static int num_inputs_multicam()
{
  return 0;
}

static int early_out_multicam(Sequence * /*seq*/, float /*fac*/)
{
  return EARLY_NO_INPUT;
}

static ImBuf *do_multicam(const SeqRenderData *context,
                          Sequence *seq,
                          float timeline_frame,
                          float /*fac*/,
                          ImBuf * /*ibuf1*/,
                          ImBuf * /*ibuf2*/,
                          ImBuf * /*ibuf3*/)
{
  ImBuf *out;
  Editing *ed;

  if (seq->multicam_source == 0 || seq->multicam_source >= seq->machine) {
    return nullptr;
  }

  ed = context->scene->ed;
  if (!ed) {
    return nullptr;
  }
  ListBase *seqbasep = SEQ_get_seqbase_by_seq(context->scene, seq);
  ListBase *channels = SEQ_get_channels_by_seq(&ed->seqbase, &ed->channels, seq);
  if (!seqbasep) {
    return nullptr;
  }

  out = seq_render_give_ibuf_seqbase(
      context, timeline_frame, seq->multicam_source, channels, seqbasep);

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Adjustment Effect
 * \{ */

/** No effect inputs for adjustment, we use #give_ibuf_seq. */
static int num_inputs_adjustment()
{
  return 0;
}

static int early_out_adjustment(Sequence * /*seq*/, float /*fac*/)
{
  return EARLY_NO_INPUT;
}

static ImBuf *do_adjustment_impl(const SeqRenderData *context, Sequence *seq, float timeline_frame)
{
  Editing *ed;
  ImBuf *i = nullptr;

  ed = context->scene->ed;

  ListBase *seqbasep = SEQ_get_seqbase_by_seq(context->scene, seq);
  ListBase *channels = SEQ_get_channels_by_seq(&ed->seqbase, &ed->channels, seq);

  /* Clamp timeline_frame to strip range so it behaves as if it had "still frame" offset (last
   * frame is static after end of strip). This is how most strips behave. This way transition
   * effects that doesn't overlap or speed effect can't fail rendering outside of strip range. */
  timeline_frame = clamp_i(timeline_frame,
                           SEQ_time_left_handle_frame_get(context->scene, seq),
                           SEQ_time_right_handle_frame_get(context->scene, seq) - 1);

  if (seq->machine > 1) {
    i = seq_render_give_ibuf_seqbase(
        context, timeline_frame, seq->machine - 1, channels, seqbasep);
  }

  /* Found nothing? so let's work the way up the meta-strip stack, so
   * that it is possible to group a bunch of adjustment strips into
   * a meta-strip and have that work on everything below the meta-strip. */

  if (!i) {
    Sequence *meta;

    meta = SEQ_find_metastrip_by_sequence(&ed->seqbase, nullptr, seq);

    if (meta) {
      i = do_adjustment_impl(context, meta, timeline_frame);
    }
  }

  return i;
}

static ImBuf *do_adjustment(const SeqRenderData *context,
                            Sequence *seq,
                            float timeline_frame,
                            float /*fac*/,
                            ImBuf * /*ibuf1*/,
                            ImBuf * /*ibuf2*/,
                            ImBuf * /*ibuf3*/)
{
  ImBuf *out;
  Editing *ed;

  ed = context->scene->ed;

  if (!ed) {
    return nullptr;
  }

  out = do_adjustment_impl(context, seq, timeline_frame);

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Speed Effect
 * \{ */

static void init_speed_effect(Sequence *seq)
{
  SpeedControlVars *v;

  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  seq->effectdata = MEM_callocN(sizeof(SpeedControlVars), "speedcontrolvars");

  v = (SpeedControlVars *)seq->effectdata;
  v->speed_control_type = SEQ_SPEED_STRETCH;
  v->speed_fader = 1.0f;
  v->speed_fader_length = 0.0f;
  v->speed_fader_frame_number = 0.0f;
}

static void load_speed_effect(Sequence *seq)
{
  SpeedControlVars *v = (SpeedControlVars *)seq->effectdata;
  v->frameMap = nullptr;
}

static int num_inputs_speed()
{
  return 1;
}

static void free_speed_effect(Sequence *seq, const bool /*do_id_user*/)
{
  SpeedControlVars *v = (SpeedControlVars *)seq->effectdata;
  if (v->frameMap) {
    MEM_freeN(v->frameMap);
  }
  MEM_SAFE_FREE(seq->effectdata);
}

static void copy_speed_effect(Sequence *dst, Sequence *src, const int /*flag*/)
{
  SpeedControlVars *v;
  dst->effectdata = MEM_dupallocN(src->effectdata);
  v = (SpeedControlVars *)dst->effectdata;
  v->frameMap = nullptr;
}

static int early_out_speed(Sequence * /*seq*/, float /*fac*/)
{
  return EARLY_DO_EFFECT;
}

static FCurve *seq_effect_speed_speed_factor_curve_get(Scene *scene, Sequence *seq)
{
  return id_data_find_fcurve(&scene->id, seq, &RNA_Sequence, "speed_factor", 0, nullptr);
}

void seq_effect_speed_rebuild_map(Scene *scene, Sequence *seq)
{
  const int effect_strip_length = SEQ_time_right_handle_frame_get(scene, seq) -
                                  SEQ_time_left_handle_frame_get(scene, seq);

  if ((seq->seq1 == nullptr) || (effect_strip_length < 1)) {
    return; /* Make COVERITY happy and check for (CID 598) input strip. */
  }

  FCurve *fcu = seq_effect_speed_speed_factor_curve_get(scene, seq);
  if (fcu == nullptr) {
    return;
  }

  SpeedControlVars *v = (SpeedControlVars *)seq->effectdata;
  if (v->frameMap) {
    MEM_freeN(v->frameMap);
  }

  v->frameMap = static_cast<float *>(MEM_mallocN(sizeof(float) * effect_strip_length, __func__));
  v->frameMap[0] = 0.0f;

  float target_frame = 0;
  for (int frame_index = 1; frame_index < effect_strip_length; frame_index++) {
    target_frame += evaluate_fcurve(fcu, SEQ_time_left_handle_frame_get(scene, seq) + frame_index);
    const int target_frame_max = SEQ_time_strip_length_get(scene, seq->seq1);
    CLAMP(target_frame, 0, target_frame_max);
    v->frameMap[frame_index] = target_frame;
  }
}

static void seq_effect_speed_frame_map_ensure(Scene *scene, Sequence *seq)
{
  SpeedControlVars *v = (SpeedControlVars *)seq->effectdata;
  if (v->frameMap != nullptr) {
    return;
  }

  seq_effect_speed_rebuild_map(scene, seq);
}

float seq_speed_effect_target_frame_get(Scene *scene,
                                        Sequence *seq_speed,
                                        float timeline_frame,
                                        int input)
{
  if (seq_speed->seq1 == nullptr) {
    return 0.0f;
  }

  SEQ_effect_handle_get(seq_speed); /* Ensure, that data are initialized. */
  int frame_index = round_fl_to_int(SEQ_give_frame_index(scene, seq_speed, timeline_frame));
  SpeedControlVars *s = (SpeedControlVars *)seq_speed->effectdata;
  const Sequence *source = seq_speed->seq1;

  float target_frame = 0.0f;
  switch (s->speed_control_type) {
    case SEQ_SPEED_STRETCH: {
      /* Only right handle controls effect speed! */
      const float target_content_length = SEQ_time_strip_length_get(scene, source) -
                                          source->startofs;
      const float speed_effetct_length = SEQ_time_right_handle_frame_get(scene, seq_speed) -
                                         SEQ_time_left_handle_frame_get(scene, seq_speed);
      const float ratio = frame_index / speed_effetct_length;
      target_frame = target_content_length * ratio;
      break;
    }
    case SEQ_SPEED_MULTIPLY: {
      FCurve *fcu = seq_effect_speed_speed_factor_curve_get(scene, seq_speed);
      if (fcu != nullptr) {
        seq_effect_speed_frame_map_ensure(scene, seq_speed);
        target_frame = s->frameMap[frame_index];
      }
      else {
        target_frame = frame_index * s->speed_fader;
      }
      break;
    }
    case SEQ_SPEED_LENGTH:
      target_frame = SEQ_time_strip_length_get(scene, source) * (s->speed_fader_length / 100.0f);
      break;
    case SEQ_SPEED_FRAME_NUMBER:
      target_frame = s->speed_fader_frame_number;
      break;
  }

  CLAMP(target_frame, 0, SEQ_time_strip_length_get(scene, source));
  target_frame += seq_speed->start;

  /* No interpolation. */
  if ((s->flags & SEQ_SPEED_USE_INTERPOLATION) == 0) {
    return target_frame;
  }

  /* Interpolation is used, switch between current and next frame based on which input is
   * requested. */
  return input == 0 ? target_frame : ceil(target_frame);
}

static float speed_effect_interpolation_ratio_get(Scene *scene,
                                                  Sequence *seq_speed,
                                                  float timeline_frame)
{
  const float target_frame = seq_speed_effect_target_frame_get(
      scene, seq_speed, timeline_frame, 0);
  return target_frame - floor(target_frame);
}

static ImBuf *do_speed_effect(const SeqRenderData *context,
                              Sequence *seq,
                              float timeline_frame,
                              float fac,
                              ImBuf *ibuf1,
                              ImBuf *ibuf2,
                              ImBuf *ibuf3)
{
  SpeedControlVars *s = (SpeedControlVars *)seq->effectdata;
  SeqEffectHandle cross_effect = get_sequence_effect_impl(SEQ_TYPE_CROSS);
  ImBuf *out;

  if (s->flags & SEQ_SPEED_USE_INTERPOLATION) {
    fac = speed_effect_interpolation_ratio_get(context->scene, seq, timeline_frame);
    /* Current frame is ibuf1, next frame is ibuf2. */
    out = seq_render_effect_execute_threaded(
        &cross_effect, context, nullptr, timeline_frame, fac, ibuf1, ibuf2, ibuf3);
    return out;
  }

  /* No interpolation. */
  return IMB_dupImBuf(ibuf1);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Over-Drop Effect
 * \{ */

static void do_overdrop_effect(const SeqRenderData *context,
                               Sequence * /*seq*/,
                               float /*timeline_frame*/,
                               float fac,
                               ImBuf *ibuf1,
                               ImBuf *ibuf2,
                               ImBuf * /*ibuf3*/,
                               int start_line,
                               int total_lines,
                               ImBuf *out)
{
  int x = context->rectx;
  int y = total_lines;

  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_drop_effect_float(fac, x, y, rect1, rect2, rect_out);
    do_alphaover_effect_float(fac, x, y, rect1, rect2, rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(
        context, ibuf1, ibuf2, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_drop_effect_byte(fac, x, y, rect1, rect2, rect_out);
    do_alphaover_effect_byte(fac, x, y, rect1, rect2, rect_out);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gaussian Blur
 * \{ */

/* NOTE: This gaussian blur implementation accumulates values in the square
 * kernel rather that doing X direction and then Y direction because of the
 * lack of using multiple-staged filters.
 *
 * Once we can we'll implement a way to apply filter as multiple stages we
 * can optimize hell of a lot in here.
 */

static void init_gaussian_blur_effect(Sequence *seq)
{
  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  seq->effectdata = MEM_callocN(sizeof(WipeVars), "wipevars");
}

static int num_inputs_gaussian_blur()
{
  return 1;
}

static void free_gaussian_blur_effect(Sequence *seq, const bool /*do_id_user*/)
{
  MEM_SAFE_FREE(seq->effectdata);
}

static void copy_gaussian_blur_effect(Sequence *dst, Sequence *src, const int /*flag*/)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
}

static int early_out_gaussian_blur(Sequence *seq, float /*fac*/)
{
  GaussianBlurVars *data = static_cast<GaussianBlurVars *>(seq->effectdata);
  if (data->size_x == 0.0f && data->size_y == 0) {
    return EARLY_USE_INPUT_1;
  }
  return EARLY_DO_EFFECT;
}

/* TODO(sergey): De-duplicate with compositor. */
static float *make_gaussian_blur_kernel(float rad, int size)
{
  float *gausstab, sum, val;
  float fac;
  int i, n;

  n = 2 * size + 1;

  gausstab = (float *)MEM_mallocN(sizeof(float) * n, __func__);

  sum = 0.0f;
  fac = (rad > 0.0f ? 1.0f / rad : 0.0f);
  for (i = -size; i <= size; i++) {
    val = RE_filter_value(R_FILTER_GAUSS, float(i) * fac);
    sum += val;
    gausstab[i + size] = val;
  }

  sum = 1.0f / sum;
  for (i = 0; i < n; i++) {
    gausstab[i] *= sum;
  }

  return gausstab;
}

static void do_gaussian_blur_effect_byte_x(Sequence *seq,
                                           int start_line,
                                           int x,
                                           int y,
                                           int frame_width,
                                           int /*frame_height*/,
                                           const uchar *rect,
                                           uchar *out)
{
#define INDEX(_x, _y) (((_y) * (x) + (_x)) * 4)
  GaussianBlurVars *data = static_cast<GaussianBlurVars *>(seq->effectdata);
  const int size_x = int(data->size_x + 0.5f);
  int i, j;

  /* Make gaussian weight table. */
  float *gausstab_x;
  gausstab_x = make_gaussian_blur_kernel(data->size_x, size_x);

  for (i = 0; i < y; i++) {
    for (j = 0; j < x; j++) {
      int out_index = INDEX(j, i);
      float accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      float accum_weight = 0.0f;

      for (int current_x = j - size_x; current_x <= j + size_x; current_x++) {
        if (current_x < 0 || current_x >= frame_width) {
          /* Out of bounds. */
          continue;
        }
        int index = INDEX(current_x, i + start_line);
        float weight = gausstab_x[current_x - j + size_x];
        accum[0] += rect[index] * weight;
        accum[1] += rect[index + 1] * weight;
        accum[2] += rect[index + 2] * weight;
        accum[3] += rect[index + 3] * weight;
        accum_weight += weight;
      }

      float inv_accum_weight = 1.0f / accum_weight;
      out[out_index + 0] = accum[0] * inv_accum_weight;
      out[out_index + 1] = accum[1] * inv_accum_weight;
      out[out_index + 2] = accum[2] * inv_accum_weight;
      out[out_index + 3] = accum[3] * inv_accum_weight;
    }
  }

  MEM_freeN(gausstab_x);
#undef INDEX
}

static void do_gaussian_blur_effect_byte_y(Sequence *seq,
                                           int start_line,
                                           int x,
                                           int y,
                                           int /*frame_width*/,
                                           int frame_height,
                                           const uchar *rect,
                                           uchar *out)
{
#define INDEX(_x, _y) (((_y) * (x) + (_x)) * 4)
  GaussianBlurVars *data = static_cast<GaussianBlurVars *>(seq->effectdata);
  const int size_y = int(data->size_y + 0.5f);
  int i, j;

  /* Make gaussian weight table. */
  float *gausstab_y;
  gausstab_y = make_gaussian_blur_kernel(data->size_y, size_y);

  for (i = 0; i < y; i++) {
    for (j = 0; j < x; j++) {
      int out_index = INDEX(j, i);
      float accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      float accum_weight = 0.0f;
      for (int current_y = i - size_y; current_y <= i + size_y; current_y++) {
        if (current_y < -start_line || current_y + start_line >= frame_height) {
          /* Out of bounds. */
          continue;
        }
        int index = INDEX(j, current_y + start_line);
        float weight = gausstab_y[current_y - i + size_y];
        accum[0] += rect[index] * weight;
        accum[1] += rect[index + 1] * weight;
        accum[2] += rect[index + 2] * weight;
        accum[3] += rect[index + 3] * weight;
        accum_weight += weight;
      }
      float inv_accum_weight = 1.0f / accum_weight;
      out[out_index + 0] = accum[0] * inv_accum_weight;
      out[out_index + 1] = accum[1] * inv_accum_weight;
      out[out_index + 2] = accum[2] * inv_accum_weight;
      out[out_index + 3] = accum[3] * inv_accum_weight;
    }
  }

  MEM_freeN(gausstab_y);
#undef INDEX
}

static void do_gaussian_blur_effect_float_x(Sequence *seq,
                                            int start_line,
                                            int x,
                                            int y,
                                            int frame_width,
                                            int /*frame_height*/,
                                            float *rect,
                                            float *out)
{
#define INDEX(_x, _y) (((_y) * (x) + (_x)) * 4)
  GaussianBlurVars *data = static_cast<GaussianBlurVars *>(seq->effectdata);
  const int size_x = int(data->size_x + 0.5f);
  int i, j;

  /* Make gaussian weight table. */
  float *gausstab_x;
  gausstab_x = make_gaussian_blur_kernel(data->size_x, size_x);

  for (i = 0; i < y; i++) {
    for (j = 0; j < x; j++) {
      int out_index = INDEX(j, i);
      float accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      float accum_weight = 0.0f;
      for (int current_x = j - size_x; current_x <= j + size_x; current_x++) {
        if (current_x < 0 || current_x >= frame_width) {
          /* Out of bounds. */
          continue;
        }
        int index = INDEX(current_x, i + start_line);
        float weight = gausstab_x[current_x - j + size_x];
        madd_v4_v4fl(accum, &rect[index], weight);
        accum_weight += weight;
      }
      mul_v4_v4fl(&out[out_index], accum, 1.0f / accum_weight);
    }
  }

  MEM_freeN(gausstab_x);
#undef INDEX
}

static void do_gaussian_blur_effect_float_y(Sequence *seq,
                                            int start_line,
                                            int x,
                                            int y,
                                            int /*frame_width*/,
                                            int frame_height,
                                            float *rect,
                                            float *out)
{
#define INDEX(_x, _y) (((_y) * (x) + (_x)) * 4)
  GaussianBlurVars *data = static_cast<GaussianBlurVars *>(seq->effectdata);
  const int size_y = int(data->size_y + 0.5f);
  int i, j;

  /* Make gaussian weight table. */
  float *gausstab_y;
  gausstab_y = make_gaussian_blur_kernel(data->size_y, size_y);

  for (i = 0; i < y; i++) {
    for (j = 0; j < x; j++) {
      int out_index = INDEX(j, i);
      float accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      float accum_weight = 0.0f;
      for (int current_y = i - size_y; current_y <= i + size_y; current_y++) {
        if (current_y < -start_line || current_y + start_line >= frame_height) {
          /* Out of bounds. */
          continue;
        }
        int index = INDEX(j, current_y + start_line);
        float weight = gausstab_y[current_y - i + size_y];
        madd_v4_v4fl(accum, &rect[index], weight);
        accum_weight += weight;
      }
      mul_v4_v4fl(&out[out_index], accum, 1.0f / accum_weight);
    }
  }

  MEM_freeN(gausstab_y);
#undef INDEX
}

static void do_gaussian_blur_effect_x_cb(const SeqRenderData *context,
                                         Sequence *seq,
                                         ImBuf *ibuf,
                                         int start_line,
                                         int total_lines,
                                         ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(
        context, ibuf, nullptr, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_gaussian_blur_effect_float_x(seq,
                                    start_line,
                                    context->rectx,
                                    total_lines,
                                    context->rectx,
                                    context->recty,
                                    ibuf->float_buffer.data,
                                    rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(
        context, ibuf, nullptr, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_gaussian_blur_effect_byte_x(seq,
                                   start_line,
                                   context->rectx,
                                   total_lines,
                                   context->rectx,
                                   context->recty,
                                   ibuf->byte_buffer.data,
                                   rect_out);
  }
}

static void do_gaussian_blur_effect_y_cb(const SeqRenderData *context,
                                         Sequence *seq,
                                         ImBuf *ibuf,
                                         int start_line,
                                         int total_lines,
                                         ImBuf *out)
{
  if (out->float_buffer.data) {
    float *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_float_buffers(
        context, ibuf, nullptr, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_gaussian_blur_effect_float_y(seq,
                                    start_line,
                                    context->rectx,
                                    total_lines,
                                    context->rectx,
                                    context->recty,
                                    ibuf->float_buffer.data,
                                    rect_out);
  }
  else {
    uchar *rect1 = nullptr, *rect2 = nullptr, *rect_out = nullptr;

    slice_get_byte_buffers(
        context, ibuf, nullptr, nullptr, out, start_line, &rect1, &rect2, nullptr, &rect_out);

    do_gaussian_blur_effect_byte_y(seq,
                                   start_line,
                                   context->rectx,
                                   total_lines,
                                   context->rectx,
                                   context->recty,
                                   ibuf->byte_buffer.data,
                                   rect_out);
  }
}

struct RenderGaussianBlurEffectInitData {
  const SeqRenderData *context;
  Sequence *seq;
  ImBuf *ibuf;
  ImBuf *out;
};

struct RenderGaussianBlurEffectThread {
  const SeqRenderData *context;
  Sequence *seq;
  ImBuf *ibuf;
  ImBuf *out;
  int start_line, tot_line;
};

static void render_effect_execute_init_handle(void *handle_v,
                                              int start_line,
                                              int tot_line,
                                              void *init_data_v)
{
  RenderGaussianBlurEffectThread *handle = (RenderGaussianBlurEffectThread *)handle_v;
  RenderGaussianBlurEffectInitData *init_data = (RenderGaussianBlurEffectInitData *)init_data_v;

  handle->context = init_data->context;
  handle->seq = init_data->seq;
  handle->ibuf = init_data->ibuf;
  handle->out = init_data->out;

  handle->start_line = start_line;
  handle->tot_line = tot_line;
}

static void *render_effect_execute_do_x_thread(void *thread_data_v)
{
  RenderGaussianBlurEffectThread *thread_data = (RenderGaussianBlurEffectThread *)thread_data_v;
  do_gaussian_blur_effect_x_cb(thread_data->context,
                               thread_data->seq,
                               thread_data->ibuf,
                               thread_data->start_line,
                               thread_data->tot_line,
                               thread_data->out);
  return nullptr;
}

static void *render_effect_execute_do_y_thread(void *thread_data_v)
{
  RenderGaussianBlurEffectThread *thread_data = (RenderGaussianBlurEffectThread *)thread_data_v;
  do_gaussian_blur_effect_y_cb(thread_data->context,
                               thread_data->seq,
                               thread_data->ibuf,
                               thread_data->start_line,
                               thread_data->tot_line,
                               thread_data->out);

  return nullptr;
}

static ImBuf *do_gaussian_blur_effect(const SeqRenderData *context,
                                      Sequence *seq,
                                      float /*timeline_frame*/,
                                      float /*fac*/,
                                      ImBuf *ibuf1,
                                      ImBuf * /*ibuf2*/,
                                      ImBuf * /*ibuf3*/)
{
  ImBuf *out = prepare_effect_imbufs(context, ibuf1, nullptr, nullptr);

  RenderGaussianBlurEffectInitData init_data;

  init_data.context = context;
  init_data.seq = seq;
  init_data.ibuf = ibuf1;
  init_data.out = out;

  IMB_processor_apply_threaded(out->y,
                               sizeof(RenderGaussianBlurEffectThread),
                               &init_data,
                               render_effect_execute_init_handle,
                               render_effect_execute_do_x_thread);

  ibuf1 = out;
  init_data.ibuf = ibuf1;
  out = prepare_effect_imbufs(context, ibuf1, nullptr, nullptr);
  init_data.out = out;

  IMB_processor_apply_threaded(out->y,
                               sizeof(RenderGaussianBlurEffectThread),
                               &init_data,
                               render_effect_execute_init_handle,
                               render_effect_execute_do_y_thread);

  IMB_freeImBuf(ibuf1);

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Effect
 * \{ */

static void init_text_effect(Sequence *seq)
{
  TextVars *data;

  if (seq->effectdata) {
    MEM_freeN(seq->effectdata);
  }

  data = static_cast<TextVars *>(seq->effectdata = MEM_callocN(sizeof(TextVars), "textvars"));
  data->text_font = nullptr;
  data->text_blf_id = -1;
  data->text_size = 60.0f;

  copy_v4_fl(data->color, 1.0f);
  data->shadow_color[3] = 0.7f;
  data->box_color[0] = 0.2f;
  data->box_color[1] = 0.2f;
  data->box_color[2] = 0.2f;
  data->box_color[3] = 0.7f;
  data->box_margin = 0.01f;

  STRNCPY(data->text, "Text");

  data->loc[0] = 0.5f;
  data->loc[1] = 0.5f;
  data->align = SEQ_TEXT_ALIGN_X_CENTER;
  data->align_y = SEQ_TEXT_ALIGN_Y_CENTER;
  data->wrap_width = 1.0f;
}

void SEQ_effect_text_font_unload(TextVars *data, const bool do_id_user)
{
  if (data == nullptr) {
    return;
  }

  /* Unlink the VFont */
  if (do_id_user && data->text_font != nullptr) {
    id_us_min(&data->text_font->id);
    data->text_font = nullptr;
  }

  /* Unload the BLF font. */
  if (data->text_blf_id >= 0) {
    BLF_unload_id(data->text_blf_id);
  }
}

void SEQ_effect_text_font_load(TextVars *data, const bool do_id_user)
{
  VFont *vfont = data->text_font;
  if (vfont == nullptr) {
    return;
  }

  if (do_id_user) {
    id_us_plus(&vfont->id);
  }

  if (vfont->packedfile != nullptr) {
    PackedFile *pf = vfont->packedfile;
    /* Create a name that's unique between library data-blocks to avoid loading
     * a font per strip which will load fonts many times.
     *
     * WARNING: this isn't fool proof!
     * The #VFont may be renamed which will cause this to load multiple times,
     * in practice this isn't so likely though. */
    char name[MAX_ID_FULL_NAME];
    BKE_id_full_name_get(name, &vfont->id, 0);

    data->text_blf_id = BLF_load_mem(name, static_cast<const uchar *>(pf->data), pf->size);
  }
  else {
    char filepath[FILE_MAX];
    STRNCPY(filepath, vfont->filepath);
    if (BLI_thread_is_main()) {
      /* FIXME: This is a band-aid fix. A proper solution has to be worked on by the VSE team.
       *
       * This code can be called from non-main thread, e.g. when copying sequences as part of
       * depsgraph CoW copy of the evaluated scene. Just skip font loading in that case, BLF code
       * is not thread-safe, and if this happens from threaded context, it almost certainly means
       * that a previous attempt to load the font already failed, e.g. because font file-path is
       * invalid. Proposer fix would likely be to not attempt to reload a failed-to-load font every
       * time. */
      BLI_path_abs(filepath, ID_BLEND_PATH_FROM_GLOBAL(&vfont->id));

      data->text_blf_id = BLF_load(filepath);
    }
  }
}

static void free_text_effect(Sequence *seq, const bool do_id_user)
{
  TextVars *data = static_cast<TextVars *>(seq->effectdata);
  SEQ_effect_text_font_unload(data, do_id_user);

  if (data) {
    MEM_freeN(data);
    seq->effectdata = nullptr;
  }
}

static void load_text_effect(Sequence *seq)
{
  TextVars *data = static_cast<TextVars *>(seq->effectdata);
  SEQ_effect_text_font_load(data, false);
}

static void copy_text_effect(Sequence *dst, Sequence *src, const int flag)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
  TextVars *data = static_cast<TextVars *>(dst->effectdata);

  data->text_blf_id = -1;
  SEQ_effect_text_font_load(data, (flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0);
}

static int num_inputs_text()
{
  return 0;
}

static int early_out_text(Sequence *seq, float /*fac*/)
{
  TextVars *data = static_cast<TextVars *>(seq->effectdata);
  if (data->text[0] == 0 || data->text_size < 1.0f ||
      ((data->color[3] == 0.0f) &&
       (data->shadow_color[3] == 0.0f || (data->flag & SEQ_TEXT_SHADOW) == 0)))
  {
    return EARLY_USE_INPUT_1;
  }
  return EARLY_NO_INPUT;
}

static ImBuf *do_text_effect(const SeqRenderData *context,
                             Sequence *seq,
                             float /*timeline_frame*/,
                             float /*fac*/,
                             ImBuf *ibuf1,
                             ImBuf *ibuf2,
                             ImBuf *ibuf3)
{
  ImBuf *out = prepare_effect_imbufs(context, ibuf1, ibuf2, ibuf3);
  TextVars *data = static_cast<TextVars *>(seq->effectdata);
  int width = out->x;
  int height = out->y;
  ColorManagedDisplay *display;
  const char *display_device;
  int font = blf_mono_font_render;
  int line_height;
  int y_ofs, x, y;
  double proxy_size_comp;

  if (data->text_blf_id == SEQ_FONT_NOT_LOADED) {
    data->text_blf_id = -1;

    SEQ_effect_text_font_load(data, false);
  }

  if (data->text_blf_id >= 0) {
    font = data->text_blf_id;
  }

  display_device = context->scene->display_settings.display_device;
  display = IMB_colormanagement_display_get_named(display_device);

  /* Compensate text size for preview render size. */
  proxy_size_comp = context->scene->r.size / 100.0;
  if (context->preview_render_size != SEQ_RENDER_SIZE_SCENE) {
    proxy_size_comp = SEQ_rendersize_to_scale_factor(context->preview_render_size);
  }

  /* set before return */
  BLF_size(font, proxy_size_comp * data->text_size);

  const int font_flags = BLF_WORD_WRAP | /* Always allow wrapping. */
                         ((data->flag & SEQ_TEXT_BOLD) ? BLF_BOLD : 0) |
                         ((data->flag & SEQ_TEXT_ITALIC) ? BLF_ITALIC : 0);
  BLF_enable(font, font_flags);

  /* use max width to enable newlines only */
  BLF_wordwrap(font, (data->wrap_width != 0.0f) ? data->wrap_width * width : -1);

  BLF_buffer(
      font, out->float_buffer.data, out->byte_buffer.data, width, height, out->channels, display);

  line_height = BLF_height_max(font);

  y_ofs = -BLF_descender(font);

  x = (data->loc[0] * width);
  y = (data->loc[1] * height) + y_ofs;

  /* vars for calculating wordwrap and optional box */
  struct {
    ResultBLF info;
    rcti rect;
  } wrap;

  BLF_boundbox_ex(font, data->text, sizeof(data->text), &wrap.rect, &wrap.info);

  if ((data->align == SEQ_TEXT_ALIGN_X_LEFT) && (data->align_y == SEQ_TEXT_ALIGN_Y_TOP)) {
    y -= line_height;
  }
  else {
    if (data->align == SEQ_TEXT_ALIGN_X_RIGHT) {
      x -= BLI_rcti_size_x(&wrap.rect);
    }
    else if (data->align == SEQ_TEXT_ALIGN_X_CENTER) {
      x -= BLI_rcti_size_x(&wrap.rect) / 2;
    }

    if (data->align_y == SEQ_TEXT_ALIGN_Y_TOP) {
      y -= line_height;
    }
    else if (data->align_y == SEQ_TEXT_ALIGN_Y_BOTTOM) {
      y += (wrap.info.lines - 1) * line_height;
    }
    else if (data->align_y == SEQ_TEXT_ALIGN_Y_CENTER) {
      y += (((wrap.info.lines - 1) / 2) * line_height) - (line_height / 2);
    }
  }

  if (data->flag & SEQ_TEXT_BOX) {
    if (out->byte_buffer.data) {
      const int margin = data->box_margin * width;
      const int minx = x + wrap.rect.xmin - margin;
      const int maxx = x + wrap.rect.xmax + margin;
      const int miny = y + wrap.rect.ymin - margin;
      const int maxy = y + wrap.rect.ymax + margin;
      IMB_rectfill_area_replace(out, data->box_color, minx, miny, maxx, maxy);
    }
  }
  /* BLF_SHADOW won't work with buffers, instead use cheap shadow trick */
  if (data->flag & SEQ_TEXT_SHADOW) {
    int fontx, fonty;
    fontx = BLF_width_max(font);
    fonty = line_height;
    BLF_position(font, x + max_ii(fontx / 55, 1), y - max_ii(fonty / 30, 1), 0.0f);
    BLF_buffer_col(font, data->shadow_color);
    BLF_draw_buffer(font, data->text, sizeof(data->text));
  }

  BLF_position(font, x, y, 0.0f);
  BLF_buffer_col(font, data->color);
  BLF_draw_buffer(font, data->text, sizeof(data->text));

  BLF_buffer(font, nullptr, nullptr, 0, 0, 0, nullptr);

  BLF_disable(font, font_flags);

  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sequence Effect Factory
 * \{ */

static void init_noop(Sequence * /*seq*/) {}

static void load_noop(Sequence * /*seq*/) {}

static void free_noop(Sequence * /*seq*/, const bool /*do_id_user*/) {}

static int num_inputs_default()
{
  return 2;
}

static void copy_effect_default(Sequence *dst, Sequence *src, const int /*flag*/)
{
  dst->effectdata = MEM_dupallocN(src->effectdata);
}

static void free_effect_default(Sequence *seq, const bool /*do_id_user*/)
{
  MEM_SAFE_FREE(seq->effectdata);
}

static int early_out_noop(Sequence * /*seq*/, float /*fac*/)
{
  return EARLY_DO_EFFECT;
}

static int early_out_fade(Sequence * /*seq*/, float fac)
{
  if (fac == 0.0f) {
    return EARLY_USE_INPUT_1;
  }
  if (fac == 1.0f) {
    return EARLY_USE_INPUT_2;
  }
  return EARLY_DO_EFFECT;
}

static int early_out_mul_input2(Sequence * /*seq*/, float fac)
{
  if (fac == 0.0f) {
    return EARLY_USE_INPUT_1;
  }
  return EARLY_DO_EFFECT;
}

static int early_out_mul_input1(Sequence * /*seq*/, float fac)
{
  if (fac == 0.0f) {
    return EARLY_USE_INPUT_2;
  }
  return EARLY_DO_EFFECT;
}

static void get_default_fac_noop(const Scene * /*scene*/,
                                 Sequence * /*seq*/,
                                 float /*timeline_frame*/,
                                 float *fac)
{
  *fac = 1.0f;
}

static void get_default_fac_fade(const Scene *scene,
                                 Sequence *seq,
                                 float timeline_frame,
                                 float *fac)
{
  *fac = float(timeline_frame - SEQ_time_left_handle_frame_get(scene, seq));
  *fac /= SEQ_time_strip_length_get(scene, seq);
}

static ImBuf *init_execution(const SeqRenderData *context,
                             ImBuf *ibuf1,
                             ImBuf *ibuf2,
                             ImBuf *ibuf3)
{
  ImBuf *out = prepare_effect_imbufs(context, ibuf1, ibuf2, ibuf3);

  return out;
}

static SeqEffectHandle get_sequence_effect_impl(int seq_type)
{
  SeqEffectHandle rval;
  int sequence_type = seq_type;

  rval.multithreaded = false;
  rval.supports_mask = false;
  rval.init = init_noop;
  rval.num_inputs = num_inputs_default;
  rval.load = load_noop;
  rval.free = free_noop;
  rval.early_out = early_out_noop;
  rval.get_default_fac = get_default_fac_noop;
  rval.execute = nullptr;
  rval.init_execution = init_execution;
  rval.execute_slice = nullptr;
  rval.copy = nullptr;

  switch (sequence_type) {
    case SEQ_TYPE_CROSS:
      rval.multithreaded = true;
      rval.execute_slice = do_cross_effect;
      rval.early_out = early_out_fade;
      rval.get_default_fac = get_default_fac_fade;
      break;
    case SEQ_TYPE_GAMCROSS:
      rval.multithreaded = true;
      rval.early_out = early_out_fade;
      rval.get_default_fac = get_default_fac_fade;
      rval.init_execution = gammacross_init_execution;
      rval.execute_slice = do_gammacross_effect;
      break;
    case SEQ_TYPE_ADD:
      rval.multithreaded = true;
      rval.execute_slice = do_add_effect;
      rval.early_out = early_out_mul_input2;
      break;
    case SEQ_TYPE_SUB:
      rval.multithreaded = true;
      rval.execute_slice = do_sub_effect;
      rval.early_out = early_out_mul_input2;
      break;
    case SEQ_TYPE_MUL:
      rval.multithreaded = true;
      rval.execute_slice = do_mul_effect;
      rval.early_out = early_out_mul_input2;
      break;
    case SEQ_TYPE_SCREEN:
    case SEQ_TYPE_OVERLAY:
    case SEQ_TYPE_COLOR_BURN:
    case SEQ_TYPE_LINEAR_BURN:
    case SEQ_TYPE_DARKEN:
    case SEQ_TYPE_LIGHTEN:
    case SEQ_TYPE_DODGE:
    case SEQ_TYPE_SOFT_LIGHT:
    case SEQ_TYPE_HARD_LIGHT:
    case SEQ_TYPE_PIN_LIGHT:
    case SEQ_TYPE_LIN_LIGHT:
    case SEQ_TYPE_VIVID_LIGHT:
    case SEQ_TYPE_BLEND_COLOR:
    case SEQ_TYPE_HUE:
    case SEQ_TYPE_SATURATION:
    case SEQ_TYPE_VALUE:
    case SEQ_TYPE_DIFFERENCE:
    case SEQ_TYPE_EXCLUSION:
      rval.multithreaded = true;
      rval.execute_slice = do_blend_mode_effect;
      rval.early_out = early_out_mul_input2;
      break;
    case SEQ_TYPE_COLORMIX:
      rval.multithreaded = true;
      rval.init = init_colormix_effect;
      rval.free = free_effect_default;
      rval.copy = copy_effect_default;
      rval.execute_slice = do_colormix_effect;
      rval.early_out = early_out_mul_input2;
      break;
    case SEQ_TYPE_ALPHAOVER:
      rval.multithreaded = true;
      rval.init = init_alpha_over_or_under;
      rval.execute_slice = do_alphaover_effect;
      rval.early_out = early_out_mul_input1;
      break;
    case SEQ_TYPE_OVERDROP:
      rval.multithreaded = true;
      rval.execute_slice = do_overdrop_effect;
      break;
    case SEQ_TYPE_ALPHAUNDER:
      rval.multithreaded = true;
      rval.init = init_alpha_over_or_under;
      rval.execute_slice = do_alphaunder_effect;
      break;
    case SEQ_TYPE_WIPE:
      rval.init = init_wipe_effect;
      rval.num_inputs = num_inputs_wipe;
      rval.free = free_wipe_effect;
      rval.copy = copy_wipe_effect;
      rval.early_out = early_out_fade;
      rval.get_default_fac = get_default_fac_fade;
      rval.execute = do_wipe_effect;
      break;
    case SEQ_TYPE_GLOW:
      rval.init = init_glow_effect;
      rval.num_inputs = num_inputs_glow;
      rval.free = free_glow_effect;
      rval.copy = copy_glow_effect;
      rval.execute = do_glow_effect;
      break;
    case SEQ_TYPE_TRANSFORM:
      rval.multithreaded = true;
      rval.init = init_transform_effect;
      rval.num_inputs = num_inputs_transform;
      rval.free = free_transform_effect;
      rval.copy = copy_transform_effect;
      rval.execute_slice = do_transform_effect;
      break;
    case SEQ_TYPE_SPEED:
      rval.init = init_speed_effect;
      rval.num_inputs = num_inputs_speed;
      rval.load = load_speed_effect;
      rval.free = free_speed_effect;
      rval.copy = copy_speed_effect;
      rval.execute = do_speed_effect;
      rval.early_out = early_out_speed;
      break;
    case SEQ_TYPE_COLOR:
      rval.init = init_solid_color;
      rval.num_inputs = num_inputs_color;
      rval.early_out = early_out_color;
      rval.free = free_solid_color;
      rval.copy = copy_solid_color;
      rval.execute = do_solid_color;
      break;
    case SEQ_TYPE_MULTICAM:
      rval.num_inputs = num_inputs_multicam;
      rval.early_out = early_out_multicam;
      rval.execute = do_multicam;
      break;
    case SEQ_TYPE_ADJUSTMENT:
      rval.supports_mask = true;
      rval.num_inputs = num_inputs_adjustment;
      rval.early_out = early_out_adjustment;
      rval.execute = do_adjustment;
      break;
    case SEQ_TYPE_GAUSSIAN_BLUR:
      rval.init = init_gaussian_blur_effect;
      rval.num_inputs = num_inputs_gaussian_blur;
      rval.free = free_gaussian_blur_effect;
      rval.copy = copy_gaussian_blur_effect;
      rval.early_out = early_out_gaussian_blur;
      rval.execute = do_gaussian_blur_effect;
      break;
    case SEQ_TYPE_TEXT:
      rval.num_inputs = num_inputs_text;
      rval.init = init_text_effect;
      rval.free = free_text_effect;
      rval.load = load_text_effect;
      rval.copy = copy_text_effect;
      rval.early_out = early_out_text;
      rval.execute = do_text_effect;
      break;
  }

  return rval;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public Sequencer Effect API
 * \{ */

SeqEffectHandle SEQ_effect_handle_get(Sequence *seq)
{
  SeqEffectHandle rval = {false, false, nullptr};

  if (seq->type & SEQ_TYPE_EFFECT) {
    rval = get_sequence_effect_impl(seq->type);
    if ((seq->flag & SEQ_EFFECT_NOT_LOADED) != 0) {
      rval.load(seq);
      seq->flag &= ~SEQ_EFFECT_NOT_LOADED;
    }
  }

  return rval;
}

SeqEffectHandle seq_effect_get_sequence_blend(Sequence *seq)
{
  SeqEffectHandle rval = {false, false, nullptr};

  if (seq->blend_mode != 0) {
    if ((seq->flag & SEQ_EFFECT_NOT_LOADED) != 0) {
      /* load the effect first */
      rval = get_sequence_effect_impl(seq->type);
      rval.load(seq);
    }

    rval = get_sequence_effect_impl(seq->blend_mode);
    if ((seq->flag & SEQ_EFFECT_NOT_LOADED) != 0) {
      /* now load the blend and unset unloaded flag */
      rval.load(seq);
      seq->flag &= ~SEQ_EFFECT_NOT_LOADED;
    }
  }

  return rval;
}

int SEQ_effect_get_num_inputs(int seq_type)
{
  SeqEffectHandle rval = get_sequence_effect_impl(seq_type);

  int count = rval.num_inputs();
  if (rval.execute || (rval.execute_slice && rval.init_execution)) {
    return count;
  }
  return 0;
}

/** \} */
