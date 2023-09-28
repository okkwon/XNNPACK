// Auto-generated file. Do not edit!
//   Template: src/qs8-gemm/MRx16c8-avx512skx.c.in
//   Generator: tools/xngen
//
// Copyright 2020 Google LLC
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>

#include <immintrin.h>

#include <xnnpack/gemm.h>
#include <xnnpack/intrinsics-polyfill.h>
#include <xnnpack/math.h>


void xnn_qd8_f32_qc4w_gemm_minmax_ukernel_1x16c8__avx512skx(
    size_t mr,
    size_t nc,
    size_t kc,
    const int8_t* restrict a,
    size_t a_stride,
    const void* restrict w,
    float* restrict c,
    size_t cm_stride,
    size_t cn_stride,
    const union xnn_f32_qc4w_minmax_params params[restrict XNN_MIN_ELEMENTS(1)],
    const struct xnn_qd8_quantization_params quantization_params[restrict XNN_MIN_ELEMENTS(1)]) XNN_OOB_READS
{
  assert(mr != 0);
  assert(mr <= 1);
  assert(nc != 0);
  assert(kc != 0);
  assert(kc % sizeof(int8_t) == 0);
  assert(a != NULL);
  assert(w != NULL);
  assert(c != NULL);

  kc = round_up_po2(kc, 8 * sizeof(int8_t));
  const int8_t* a0 = a;
  float* c0 = c;

  const __mmask16 vbias_mask = _cvtu32_mask16(0x1111);
  const __m512i vinput_zero_point0 = _mm512_set1_epi32((int) quantization_params[0].zero_point);
  const __m512 voutput_min = _mm512_set1_ps(params->scalar.min);
  const __m512 voutput_max = _mm512_set1_ps(params->scalar.max);
  const __m128i vmask = _mm_set1_epi8(UINT8_C(0xF0));
  do {
    const __m512i vksum0123 = _mm512_maskz_expandloadu_epi32(vbias_mask, w);
    const __m512i vksum4567 = _mm512_maskz_expandloadu_epi32(vbias_mask, (const int32_t*) w + 4);
    const __m512i vksum89AB = _mm512_maskz_expandloadu_epi32(vbias_mask, (const int32_t*) w + 8);
    const __m512i vksumCDEF = _mm512_maskz_expandloadu_epi32(vbias_mask, (const int32_t*) w + 12);

    __m512i vacc0x0123 = _mm512_mullo_epi32(vksum0123, vinput_zero_point0);
    __m512i vacc0x4567 = _mm512_mullo_epi32(vksum4567, vinput_zero_point0);
    __m512i vacc0x89AB = _mm512_mullo_epi32(vksum89AB, vinput_zero_point0);
    __m512i vacc0xCDEF = _mm512_mullo_epi32(vksumCDEF, vinput_zero_point0);
    w = (const int32_t*) w + 16;

    size_t k = 0;
    // Accumulate blocks multiplication for each row.
    while (k < kc) {
      const __m512i va0 = _mm512_broadcast_i32x4(_mm_cvtepi8_epi16(_mm_loadl_epi64((const __m128i*) a0)));
      a0 += 8;

      const __m128i vbb0123 = _mm_load_si128((const __m128i*) ((const int8_t*) w + 0));
      const __m128i vbs01 = _mm_slli_epi32(vbb0123, 4);
      const __m128i vbb01 = _mm_and_si128(vbs01, vmask);
      const __m128i vbb23 = _mm_and_si128(vbb0123, vmask);
      const __m256i vwb01 = _mm256_cvtepi8_epi16(vbb01);
      const __m256i vwb23 = _mm256_cvtepi8_epi16(vbb23);
      const __m512i vxb01 = _mm512_cvtepu16_epi32(vwb01);
      const __m512i vxb23 = _mm512_cvtepu16_epi32(vwb23);
      const __m512i vxsb23 = _mm512_slli_epi32(vxb23, 16);
      const __m512i vb0123 = _mm512_or_si512(vxb01, vxsb23);

      vacc0x0123 = _mm512_add_epi32(vacc0x0123, _mm512_madd_epi16(va0, vb0123));
      const __m128i vbb4567 = _mm_load_si128((const __m128i*) ((const int8_t*) w + 16));
      const __m128i vbs45 = _mm_slli_epi32(vbb4567, 4);
      const __m128i vbb45 = _mm_and_si128(vbs45, vmask);
      const __m128i vbb67 = _mm_and_si128(vbb4567, vmask);
      const __m256i vwb45 = _mm256_cvtepi8_epi16(vbb45);
      const __m256i vwb67 = _mm256_cvtepi8_epi16(vbb67);
      const __m512i vxb45 = _mm512_cvtepu16_epi32(vwb45);
      const __m512i vxb67 = _mm512_cvtepu16_epi32(vwb67);
      const __m512i vxsb67 = _mm512_slli_epi32(vxb67, 16);
      const __m512i vb4567 = _mm512_or_si512(vxb45, vxsb67);

      vacc0x4567 = _mm512_add_epi32(vacc0x4567, _mm512_madd_epi16(va0, vb4567));
      const __m128i vbb89AB = _mm_load_si128((const __m128i*) ((const int8_t*) w + 32));
      const __m128i vbs89 = _mm_slli_epi32(vbb89AB, 4);
      const __m128i vbb89 = _mm_and_si128(vbs89, vmask);
      const __m128i vbbAB = _mm_and_si128(vbb89AB, vmask);
      const __m256i vwb89 = _mm256_cvtepi8_epi16(vbb89);
      const __m256i vwbAB = _mm256_cvtepi8_epi16(vbbAB);
      const __m512i vxb89 = _mm512_cvtepu16_epi32(vwb89);
      const __m512i vxbAB = _mm512_cvtepu16_epi32(vwbAB);
      const __m512i vxsbAB = _mm512_slli_epi32(vxbAB, 16);
      const __m512i vb89AB = _mm512_or_si512(vxb89, vxsbAB);

      vacc0x89AB = _mm512_add_epi32(vacc0x89AB, _mm512_madd_epi16(va0, vb89AB));
      const __m128i vbbCDEF = _mm_load_si128((const __m128i*) ((const int8_t*) w + 48));
      const __m128i vbsCD = _mm_slli_epi32(vbbCDEF, 4);
      const __m128i vbbCD = _mm_and_si128(vbsCD, vmask);
      const __m128i vbbEF = _mm_and_si128(vbbCDEF, vmask);
      const __m256i vwbCD = _mm256_cvtepi8_epi16(vbbCD);
      const __m256i vwbEF = _mm256_cvtepi8_epi16(vbbEF);
      const __m512i vxbCD = _mm512_cvtepu16_epi32(vwbCD);
      const __m512i vxbEF = _mm512_cvtepu16_epi32(vwbEF);
      const __m512i vxsbEF = _mm512_slli_epi32(vxbEF, 16);
      const __m512i vbCDEF = _mm512_or_si512(vxbCD, vxsbEF);

      vacc0xCDEF = _mm512_add_epi32(vacc0xCDEF, _mm512_madd_epi16(va0, vbCDEF));

      w = (const int8_t*) w + 64;
      k += 8 * sizeof(int8_t);
    }

    const __m512i vacc0x04152637 = _mm512_add_epi32(_mm512_unpacklo_epi32(vacc0x0123, vacc0x4567), _mm512_unpackhi_epi32(vacc0x0123, vacc0x4567));
    const __m512i vacc0x8C9DAEBF = _mm512_add_epi32(_mm512_unpacklo_epi32(vacc0x89AB, vacc0xCDEF), _mm512_unpackhi_epi32(vacc0x89AB, vacc0xCDEF));

    __m512i vacc0x084C195D2A6E3B7F = _mm512_add_epi32(_mm512_unpacklo_epi32(vacc0x04152637, vacc0x8C9DAEBF), _mm512_unpackhi_epi32(vacc0x04152637, vacc0x8C9DAEBF));

    vacc0x084C195D2A6E3B7F = _mm512_srai_epi32(vacc0x084C195D2A6E3B7F, 4);
    __m512 vscaled0x084C195D2A6E3B7F = _mm512_cvtepi32_ps(vacc0x084C195D2A6E3B7F);

    __m512 vout0x0123456789ABCDEF = _mm512_permutexvar_ps(_mm512_set_epi32(15, 11, 7, 3, 13, 9, 5, 1, 14, 10, 6, 2, 12, 8, 4, 0), vscaled0x084C195D2A6E3B7F);

    vout0x0123456789ABCDEF = _mm512_mul_ps(vout0x0123456789ABCDEF, _mm512_set1_ps(quantization_params[0].inv_scale));

    const __m512 vfilter_output_scale0123456789ABCDEF = _mm512_load_ps((const float*) w);
    const __m512 vbias0123456789ABCDEF = _mm512_load_ps((const float*) w + 16);
    w = (const float*) w + 32;
    vout0x0123456789ABCDEF = _mm512_fmadd_ps(vout0x0123456789ABCDEF, vfilter_output_scale0123456789ABCDEF, vbias0123456789ABCDEF);

    vout0x0123456789ABCDEF = _mm512_max_ps(vout0x0123456789ABCDEF, voutput_min);

    vout0x0123456789ABCDEF = _mm512_min_ps(vout0x0123456789ABCDEF, voutput_max);

    if (nc >= 16) {
      _mm512_storeu_ps(c0, vout0x0123456789ABCDEF);

      a0 = (const int8_t*) ((uintptr_t) a0 - k);

      c0 = (float*) ((uintptr_t) c0 + cn_stride);

      nc -= 16;
    } else {
      // Prepare mask for valid 32-bit elements (depends on nc).
      const __mmask16 vmask = _cvtu32_mask16((UINT32_C(1) << nc) - 1);
      _mm512_mask_storeu_ps(c0, vmask, vout0x0123456789ABCDEF);

      nc = 0;
    }
  } while (nc != 0);
}