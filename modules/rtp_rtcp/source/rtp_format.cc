/*
 *  Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/rtp_rtcp/source/rtp_format.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "api/array_view.h"
#include "api/video/video_codec_type.h"
#include "modules/rtp_rtcp/source/rtp_format_h264.h"
#include "modules/rtp_rtcp/source/rtp_format_video_generic.h"
#include "modules/rtp_rtcp/source/rtp_format_vp8.h"
#include "modules/rtp_rtcp/source/rtp_format_vp9.h"
#include "modules/rtp_rtcp/source/rtp_packetizer_av1.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "modules/video_coding/codecs/vp8/include/vp8_globals.h"
#include "modules/video_coding/codecs/vp9/include/vp9_globals.h"
#include "rtc_base/checks.h"
#ifdef RTC_ENABLE_H265
#include "modules/rtp_rtcp/source/rtp_packetizer_h265.h"
#endif

namespace webrtc {

std::unique_ptr<RtpPacketizer> RtpPacketizer::Create(
    PacketizationFormat format,
    ArrayView<const uint8_t> payload,
    PayloadSizeLimits limits,
    // Codec-specific details.
    const RTPVideoHeader& rtp_video_header) {
  using enum PacketizationFormat;
  switch (format) {
    case kRaw: {
      return std::make_unique<RtpPacketizerGeneric>(payload, limits);
    }
    case kH264: {
      const auto& h264 =
          std::get<RTPVideoHeaderH264>(rtp_video_header.video_type_header);
      return std::make_unique<RtpPacketizerH264>(payload, limits,
                                                 h264.packetization_mode);
    }
    case kVP8: {
      const auto& vp8 =
          std::get<RTPVideoHeaderVP8>(rtp_video_header.video_type_header);
      return std::make_unique<RtpPacketizerVp8>(payload, limits, vp8);
    }
    case kVP9: {
      const auto& vp9 =
          std::get<RTPVideoHeaderVP9>(rtp_video_header.video_type_header);
      return std::make_unique<RtpPacketizerVp9>(payload, limits, vp9);
    }
    case kAV1:
      return std::make_unique<RtpPacketizerAv1>(
          payload, limits, rtp_video_header.frame_type,
          rtp_video_header.is_last_frame_in_picture);
    case kH265: {
#ifdef RTC_ENABLE_H265
      return std::make_unique<RtpPacketizerH265>(payload, limits);
#else
      return std::make_unique<RtpPacketizerGeneric>(payload, limits,
                                                    rtp_video_header);
#endif
    }
    case kGeneric: {
      return std::make_unique<RtpPacketizerGeneric>(payload, limits,
                                                    rtp_video_header);
    }
  }
}

std::unique_ptr<RtpPacketizer> RtpPacketizer::Create(
    std::optional<VideoCodecType> type,
    ArrayView<const uint8_t> payload,
    PayloadSizeLimits limits,
    // Codec-specific details.
    const RTPVideoHeader& rtp_video_header) {
  if (!type) {
    return Create(PacketizationFormat::kRaw, payload, limits, rtp_video_header);
  }
  switch (*type) {
    case kVideoCodecH264:
      return Create(PacketizationFormat::kH264, payload, limits,
                    rtp_video_header);
    case kVideoCodecVP8:
      return Create(PacketizationFormat::kVP8, payload, limits,
                    rtp_video_header);
    case kVideoCodecVP9:
      return Create(PacketizationFormat::kVP9, payload, limits,
                    rtp_video_header);
    case kVideoCodecAV1:
      return Create(PacketizationFormat::kAV1, payload, limits,
                    rtp_video_header);
    case kVideoCodecH265:
      return Create(PacketizationFormat::kH265, payload, limits,
                    rtp_video_header);
    default:
      return Create(PacketizationFormat::kGeneric, payload, limits,
                    rtp_video_header);
  }
}

std::vector<int> RtpPacketizer::SplitAboutEqually(
    int payload_len,
    const PayloadSizeLimits& limits) {
  RTC_DCHECK_GT(payload_len, 0);
  // First or last packet larger than normal are unsupported.
  RTC_DCHECK_GE(limits.first_packet_reduction_len, 0);
  RTC_DCHECK_GE(limits.last_packet_reduction_len, 0);

  std::vector<int> result;
  if (limits.max_payload_len >=
      limits.single_packet_reduction_len + payload_len) {
    result.push_back(payload_len);
    return result;
  }
  if (limits.max_payload_len - limits.first_packet_reduction_len < 1 ||
      limits.max_payload_len - limits.last_packet_reduction_len < 1) {
    // Capacity is not enough to put a single byte into one of the packets.
    return result;
  }
  // First and last packet of the frame can be smaller. Pretend that it's
  // the same size, but we must write more payload to it.
  // Assume frame fits in single packet if packet has extra space for sum
  // of first and last packets reductions.
  int total_bytes = payload_len + limits.first_packet_reduction_len +
                    limits.last_packet_reduction_len;
  // Integer divisions with rounding up.
  int num_packets_left =
      (total_bytes + limits.max_payload_len - 1) / limits.max_payload_len;
  if (num_packets_left == 1) {
    // Single packet is a special case handled above.
    num_packets_left = 2;
  }

  if (payload_len < num_packets_left) {
    // Edge case where limits force to have more packets than there are payload
    // bytes. This may happen when there is single byte of payload that can't be
    // put into single packet if
    // first_packet_reduction + last_packet_reduction >= max_payload_len.
    return result;
  }

  int bytes_per_packet = total_bytes / num_packets_left;
  int num_larger_packets = total_bytes % num_packets_left;
  int remaining_data = payload_len;

  result.reserve(num_packets_left);
  bool first_packet = true;
  while (remaining_data > 0) {
    // Last num_larger_packets are 1 byte wider than the rest. Increase
    // per-packet payload size when needed.
    if (num_packets_left == num_larger_packets)
      ++bytes_per_packet;
    int current_packet_bytes = bytes_per_packet;
    if (first_packet) {
      if (current_packet_bytes > limits.first_packet_reduction_len + 1)
        current_packet_bytes -= limits.first_packet_reduction_len;
      else
        current_packet_bytes = 1;
    }
    if (current_packet_bytes > remaining_data) {
      current_packet_bytes = remaining_data;
    }
    // This is not the last packet in the whole payload, but there's no data
    // left for the last packet. Leave at least one byte for the last packet.
    if (num_packets_left == 2 && current_packet_bytes == remaining_data) {
      --current_packet_bytes;
    }
    result.push_back(current_packet_bytes);

    remaining_data -= current_packet_bytes;
    --num_packets_left;
    first_packet = false;
  }

  return result;
}

}  // namespace webrtc
