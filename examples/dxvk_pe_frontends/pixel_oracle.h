// SPDX-License-Identifier: MIT
// A CPU readback of the centre pixel, independent of Present's HRESULT.
#pragma once
#include <cstdint>
#include <cstdio>

static unsigned pe_pixel_mismatches(const void* pixel, unsigned frame,
                                    bool check_alpha) {
  const auto* bgra = static_cast<const std::uint8_t*>(pixel);
  const std::uint8_t red = frame ? 132 : 28;
  const std::uint8_t blue = frame ? 28 : 132;
  return unsigned(bgra[0] != blue) + unsigned(bgra[1] != 76) +
         unsigned(bgra[2] != red) +
         (check_alpha ? unsigned(bgra[3] != 255) : 0);
}

static unsigned pe_log_pixel(const char* api, unsigned frame, const void* pixel,
                             bool check_alpha) {
  const auto* bgra = static_cast<const std::uint8_t*>(pixel);
  const unsigned mismatches = pe_pixel_mismatches(pixel, frame, check_alpha);
  std::printf("DXVK_PE_%s_PIXEL frame=%u bgra=%02x%02x%02x%02x mismatches=%u\n",
              api, frame, unsigned(bgra[0]), unsigned(bgra[1]),
              unsigned(bgra[2]), unsigned(bgra[3]), mismatches);
  return mismatches;
}
