// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of TeleMatrix, licensed under the GNU GPL version 3
// or later, with an OpenSSL linking exception. See the LICENSE and LEGAL
// files in the project root for full terms.

//! Local video frame extraction into a JPEG thumbnail using ffmpeg (libav).
//!
//! Given a decrypted local video file this opens the best video stream, seeks
//! near the start, decodes a single frame, scales it to fit the requested
//! box (preserving aspect ratio), and re-encodes it as a baseline JPEG. All
//! ffmpeg work is synchronous/CPU-bound, so callers run it on a blocking task.

use std::path::Path;
use std::sync::Once;

use anyhow::{anyhow, Context as _, Result};
use ffmpeg::format::Pixel;
use ffmpeg::media::Type;
use ffmpeg::software::scaling::{context::Context as Scaler, flag::Flags};
use ffmpeg::util::frame::video::Video as VideoFrame;
use ffmpeg_next as ffmpeg;

static FFMPEG_INIT: Once = Once::new();

/// Seek roughly this far into the clip so we skip black intro frames while
/// staying cheap. The decoder still falls back to the first decodable frame if
/// seeking lands past the only keyframe.
const SEEK_TARGET_SECONDS: f64 = 1.0;
const JPEG_QSCALE: i32 = 4; // 1 (best) .. 31 (worst); 4 ~ visually clean preview.

/// Decode one frame from `video_path`, scale it to fit `max_width`×`max_height`
/// (aspect-preserving, never upscaling), and return baseline JPEG bytes.
pub fn extract_thumbnail_jpeg(
    video_path: &Path,
    max_width: u32,
    max_height: u32,
) -> Result<Vec<u8>> {
    FFMPEG_INIT.call_once(|| {
        // ffmpeg::init() registers codecs/formats. It only needs to run once
        // per process and is safe to call from any thread behind this guard.
        let _ = ffmpeg::init();
    });

    let max_width = max_width.max(1);
    let max_height = max_height.max(1);

    let mut ictx = ffmpeg::format::input(&video_path)
        .with_context(|| format!("ffmpeg: open input {}", video_path.display()))?;

    let stream = ictx
        .streams()
        .best(Type::Video)
        .ok_or_else(|| anyhow!("ffmpeg: no video stream in {}", video_path.display()))?;
    let stream_index = stream.index();
    let time_base = stream.time_base();

    let decoder_ctx = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
        .context("ffmpeg: build decoder context")?;
    let mut decoder = decoder_ctx
        .decoder()
        .video()
        .context("ffmpeg: open video decoder")?;

    // Best-effort seek ~1s in. time_base is ticks/second; convert seconds to a
    // stream timestamp. Failure is non-fatal — we just decode from the start.
    if time_base.numerator() > 0 {
        let tb = f64::from(time_base.numerator()) / f64::from(time_base.denominator());
        if tb > 0.0 {
            let target_ts = (SEEK_TARGET_SECONDS / tb) as i64;
            if target_ts > 0 {
                let _ = ictx.seek(target_ts, ..target_ts);
                decoder.flush();
            }
        }
    }

    let src_w = decoder.width();
    let src_h = decoder.height();
    if src_w == 0 || src_h == 0 {
        return Err(anyhow!("ffmpeg: decoder reported zero dimensions"));
    }
    let (dst_w, dst_h) = fit_dimensions(src_w, src_h, max_width, max_height);
    tracing::debug!(
        "[VIDTHUMB] decode {} — source {src_w}x{src_h} -> thumb {dst_w}x{dst_h}",
        video_path.display()
    );

    let decoded = decode_first_frame(&mut ictx, &mut decoder, stream_index)?;

    // Scale + convert to the JPEG-friendly full-range YUV420 layout.
    let mut scaler = Scaler::get(
        decoder.format(),
        src_w,
        src_h,
        Pixel::YUVJ420P,
        dst_w,
        dst_h,
        Flags::BILINEAR,
    )
    .context("ffmpeg: build scaler")?;
    let mut scaled = VideoFrame::empty();
    scaler
        .run(&decoded, &mut scaled)
        .context("ffmpeg: scale frame")?;
    scaled.set_pts(Some(0));

    encode_jpeg(&scaled, dst_w, dst_h)
}

/// Pull packets from the video stream until the decoder yields one frame.
fn decode_first_frame(
    ictx: &mut ffmpeg::format::context::Input,
    decoder: &mut ffmpeg::decoder::Video,
    stream_index: usize,
) -> Result<VideoFrame> {
    let mut frame = VideoFrame::empty();
    for (stream, packet) in ictx.packets() {
        if stream.index() != stream_index {
            continue;
        }
        decoder
            .send_packet(&packet)
            .context("ffmpeg: send packet to decoder")?;
        if decoder.receive_frame(&mut frame).is_ok() {
            return Ok(frame);
        }
    }
    // Drain: the only frame may still be buffered after EOF.
    decoder.send_eof().context("ffmpeg: decoder send eof")?;
    if decoder.receive_frame(&mut frame).is_ok() {
        return Ok(frame);
    }
    Err(anyhow!("ffmpeg: no decodable video frame"))
}

/// Encode a single YUVJ420P frame as a baseline JPEG via the mjpeg encoder.
fn encode_jpeg(frame: &VideoFrame, width: u32, height: u32) -> Result<Vec<u8>> {
    let codec = ffmpeg::encoder::find(ffmpeg::codec::Id::MJPEG)
        .ok_or_else(|| anyhow!("ffmpeg: mjpeg encoder unavailable"))?;

    let mut enc = ffmpeg::codec::context::Context::new_with_codec(codec)
        .encoder()
        .video()
        .context("ffmpeg: build mjpeg encoder context")?;
    enc.set_width(width);
    enc.set_height(height);
    enc.set_format(Pixel::YUVJ420P);
    // mjpeg has no real frame rate; a 1-fps time base keeps it valid.
    enc.set_time_base(ffmpeg::Rational(1, 1));
    // Quantizer scale controls JPEG quality for the mjpeg encoder.
    enc.set_qmin(JPEG_QSCALE);
    enc.set_qmax(JPEG_QSCALE);

    let mut encoder = enc.open().context("ffmpeg: open mjpeg encoder")?;

    encoder
        .send_frame(frame)
        .context("ffmpeg: send frame to encoder")?;
    encoder.send_eof().context("ffmpeg: encoder send eof")?;

    let mut out = Vec::new();
    let mut packet = ffmpeg::Packet::empty();
    while encoder.receive_packet(&mut packet).is_ok() {
        if let Some(data) = packet.data() {
            out.extend_from_slice(data);
        }
    }
    if out.is_empty() {
        return Err(anyhow!("ffmpeg: mjpeg encoder produced no output"));
    }
    Ok(out)
}

/// Scale (src_w, src_h) to fit inside (max_w, max_h) preserving aspect ratio,
/// never upscaling. Dimensions are forced even for chroma-subsampled output.
fn fit_dimensions(src_w: u32, src_h: u32, max_w: u32, max_h: u32) -> (u32, u32) {
    let scale = (f64::from(max_w) / f64::from(src_w))
        .min(f64::from(max_h) / f64::from(src_h))
        .min(1.0);
    let w = ((f64::from(src_w) * scale).round() as u32).max(2);
    let h = ((f64::from(src_h) * scale).round() as u32).max(2);
    // YUV420 needs even dimensions.
    (w & !1, h & !1)
}

#[cfg(test)]
mod tests {
    use super::fit_dimensions;

    #[test]
    fn fit_preserves_aspect_and_never_upscales() {
        assert_eq!(fit_dimensions(1920, 1080, 320, 320), (320, 180));
        assert_eq!(fit_dimensions(100, 100, 320, 320), (100, 100));
        assert_eq!(fit_dimensions(1080, 1920, 320, 320), (180, 320));
    }

    #[test]
    fn fit_dimensions_are_even() {
        let (w, h) = fit_dimensions(1001, 667, 300, 300);
        assert_eq!(w & 1, 0);
        assert_eq!(h & 1, 0);
    }
}
