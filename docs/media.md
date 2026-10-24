# Media pipeline (app/src/media)

Owner: media. Implements `MediaPipeline` from `docs/contract.md` section 3: the `IVideoSink` + `IAudioSink` that
`ReceiverHost` feeds, plus the view controls `MainPage` drives. Goal: lowest glass-to-glass latency for iPhone
AirPlay mirroring, with good picture quality and Auto crop of letterboxed content.

## Layout
```
app/src/media/
  MediaPipeline.{h,cpp}   the contract class; owns the device and the three threads, /status "media" provider
  D3DDevice.{h,cpp}       shared D3D11 device (VIDEO_SUPPORT|BGRA, multithread protected), D3D11VA device ctx, loss
  VideoDecoder.{h,cpp}    decode thread: FIFO -> FFmpeg (d3d11va or software) -> renderer mailbox
  VideoRenderer.{h,cpp}   render thread: composition swap chain, YUV->RGB, view transform, crop readback
  VideoFrame.h            decoded picture handed decode -> render (holds its AVFrame / decoder surface)
  AudioOutput.{h,cpp}     audio thread: FFmpeg decode -> swresample -> XAudio2 source voice, A/V timing
  MediaPlatform.h         Windows-only includes (D3D11, DXGI, XAudio2, FFmpeg hwcontext)
  shaders/                video_vs.hlsl, video_ps.hlsl, crop_ps.hlsl, video.hlsli (FxCompile -> shaders/*.h)
  portable/               no Windows headers: crop detection, view transform, colour matrices, SPS rewrite,
                          FFmpeg decode wrappers, audio scheduler, queues, stats JSON. Compiled by the app AND the
                          Linux harness.
  test/                   Linux harness (CMake). NOT part of the UWP build, see "Requests to platform".
```

## Pipeline
```
 castcore threads              decode thread                 render thread                    XAML compositor
 ----------------              -------------                 -------------                    ---------------
 OnFormat(codec,w,h) ─┐
 OnFrame(AU) ─copy──▶ FIFO(240) ─▶ SPS rewrite ─▶ avcodec ─▶ mailbox(1) ─▶ wait swap chain slot ─▶ SwapChainPanel
 OnFlush ─────────────┘          (reorder=0)    d3d11va      newest wins    draw NV12/P010 -> RGB     (flip, 1 queued)
                                                 (or sw)                     Present(1,0)
                                                                             every 500 ms: luma -> 256x144 R8
                                                                               -> staging -> CropDetector

 OnFormat(asc/cookie) ┐          audio thread
 OnPacket ─copy──────▶ FIFO(256) ─▶ avcodec (ELD/ALAC/LC/PCM) ─▶ swr -> float stereo @ device rate
 OnFlush / OnVolume ──┘            ─▶ AudioScheduler (pts -> write cursor) ─▶ XAudio2 source voice ─▶ mastering voice
```
Threads: castcore calls return after one copy (contract: ~1 ms budget). One decode thread (ABOVE_NORMAL), one
render thread (ABOVE_NORMAL), one audio thread (TIME_CRITICAL) plus XAudio2's own mixer thread. The UI thread only
calls setters (atomics / short mutex) and receives the swap chain through `ISwapChainPanelNative::SetSwapChain`, which
the render thread posts to the Dispatcher (never waits on it, so `Stop()` on the UI thread cannot deadlock).

Lifecycle: `MediaPipeline` is default-constructible. `Attach(panel)` subscribes to `SizeChanged` and
`CompositionScaleChanged`. Threads start lazily on the first sink call of a session; `Stop()` joins them, releases
the decoder, swap chain (unbound from the panel), XAudio2 voices and the D3D device: idle holds no GPU memory.

## Video decode
- FFmpeg `h264` / `hevc` decoders. Hardware path: `AV_HWDEVICE_TYPE_D3D11VA` wrapping OUR device
  (`AVD3D11VADeviceContext` with our device/context and lock callbacks on the device's recursive mutex, as
  moonlight-xbox does). The frame pool is created by us (`avcodec_get_hw_frames_parameters` + `BindFlags |=
  SHADER_RESOURCE`) so the renderer samples the decoder surfaces directly: no copy, no video processor.
- HEVC Main and Main10 are first-class (owner decision: `hevcPlayback` capability). Main10 decodes to P010; the
  renderer samples it as R16/R16G16 and converts with the 10-bit matrix. Nothing assumes 1080p: a 4K source decodes
  at source resolution and is scaled on the GPU into the panel-sized swap chain.
- Low delay: `AV_CODEC_FLAG_LOW_DELAY`, `AV_CODEC_FLAG2_FAST`, `thread_count = 1` (frame threading adds a frame of
  delay per thread), `has_b_frames = 0`.
- **iOS SPS fix** (the 130-700 ms trap documented by screen-mirror-xbox): the iPhone's H.264 SPS has VUI without
  `bitstream_restriction`, so a conforming decoder may hold up to the DPB size before output.
  `portable/H264Sps` rewrites every SPS in the stream to add `bitstream_restriction` with
  `max_num_reorder_frames = 0` and `max_dec_frame_buffering = max_num_ref_frames`. Exported as
  `video.spsRewritten`. Every frame's packet-in -> picture-out time is measured (`decodeInOutMs*`) and the most
  packets a picture waited behind is tracked (`decodeHeldMax`, must be 0); a held picture logs a warning.
- Queue policy: every access unit is decoded (mirroring is one endless GOP; dropping a reference corrupts the picture
  until an IDR that we cannot request). If more AUs are already queued, the decoded picture is not handed to the
  renderer (`droppedRender`), so a backlog drains at decode speed. Only a FIFO overflow (decoder stalled for seconds)
  drops the backlog and waits for the next AU with parameter sets (`droppedQueue`).
- Software fallback: if the D3D11VA device cannot be created, the decoder open fails, or `get_format` is not offered
  `AV_PIX_FMT_D3D11` for this stream, the decoder runs in software (logged as `SOFTWARE decode (...)`), and the
  renderer uploads yuv420p / yuv420p10 / nv12 / p010 frames into dynamic textures.
- Format change: `OnFormat` with a new codec or size (rotation sends a new stream header, so a portrait phone at
  ~500x1080 becomes landscape) reopens the decoder; frames whose own codec/size disagree with the current format also
  trigger a reopen. `OnFlush` drops the queue, flushes the decoder and waits for a keyframe. Both reset the renderer's
  layout so the new geometry snaps instead of animating from the old aspect.
- Device loss: the decoder notices the device generation change, reopens on the new device and waits for a keyframe.

## Video render
- `CreateSwapChainForComposition`, `B8G8R8A8_UNORM`, `FLIP_SEQUENTIAL`, 2 buffers,
  `FRAME_LATENCY_WAITABLE_OBJECT` + `SetMaximumFrameLatency(1)`. Sized to panel `ActualSize x CompositionScale`;
  the inverse scale goes on `IDXGISwapChain2::SetMatrixTransform` (moonlight-xbox does the same) so the swap chain is
  1:1 with physical pixels. Resized with `ResizeBuffers` on `SizeChanged`/`CompositionScaleChanged`.
- Render loop: wait on the frame-latency object (a buffer is free), then wait for the newest picture in the
  mailbox / an animation tick / a parameter change, draw, `Present(1, 0)`. Waiting for the slot FIRST and the picture
  SECOND means the picture we draw is always the newest one available at the moment a buffer can take it.
- Present: `Present(1, 0)` (vsync) is the default. With max latency 1 and a waitable swap chain, a picture that
  arrives just after vblank waits at most one refresh (16.7 ms at 60 Hz); `Present(0, 0)` cannot tear on a
  composition swap chain (DWM composes at vblank anyway) and would only let us submit faster than the compositor
  consumes. `SetSyncInterval(0)` exists for an on-console A/B measurement (`presentMsAvg`, `latencyMsAvg`).
- Shader: one quad from `SV_VertexID` (no vertex buffer); destination rect in NDC and source rect in UV come from the
  view transform. Pixel shader: YUV -> RGB with the stream's matrix (BT.709 / BT.601 / BT.2020 from the VUI
  `colorspace`, limited or full range from `color_range`; unspecified -> BT.709 limited for HD, BT.601 for SD).
  Luma uses Catmull-Rom bicubic (9 bilinear taps) when upscaling, bilinear otherwise; chroma bilinear. Black clear
  around the content. 10 fetches/pixel at most: negligible on Series S.
- Device lost (`DXGI_ERROR_DEVICE_REMOVED/RESET/HUNG` from Present/ResizeBuffers or the removed-reason check): every
  GPU resource and the swap chain are released, the pipeline creates a new device (retry once a second), the
  decoder reopens on it.

## View modes and Auto
`portable/ViewTransform` turns (frame size, content rect, view size, mode, zoom, pan) into the destination quad and
source UV rect. Transitions between any two layouts animate with a 300 ms ease-out cubic in the render thread; a new
stream (format change / flush) snaps.
- **Fit**: the whole (cropped, if auto-crop is on) content inside the view, black bars around.
- **Fill**: covers the view, crops the overflow (centred).
- **Zoom**: Fit scaled by `zoom` (1.0..3.0) around a pan centre. `Pan(dx, dy)` moves the centre by a fraction of the
  visible area and is clamped so the content always covers what it can (`Pan(-10,-10)` lands at the top-left
  limit). `ResetPan()` centres.
- **Auto**: crop the detected black bars (when auto-crop is on), then Fill if Fill hides at most 8 % of the cropped
  content (`autoFillMaxLoss = 0.08`), else Fit. `/status` reports what Auto resolved to (`mode: "Auto (Fill)"`).

Why crop matters: the iPhone never pads its stream to our aspect. A portrait phone arrives ~500x1080 and a landscape
one ~1920x886; what users see as "black bars" is letterboxing INSIDE the phone frame (a 16:9 video playing in a
portrait app, a 2.39:1 film in a landscape phone). Cropping those bars and then filling is what makes the picture
fill the TV. Example from the harness: a 16:9 video inside a portrait phone frame goes from scale 1.0 (Fit, no crop)
to 3.87 (Auto).

## Crop detection
Every 500 ms the render thread draws the visible luma into a 256x144 R8 target (each output pixel = 16 bilinear taps
over its footprint, close to a box filter), copies it into a staging texture, and maps it on a later frame with
`D3D11_MAP_FLAG_DO_NOT_WAIT`, so the GPU never stalls. `portable/CropDetector`:
- a row/column is a bar when its mean is within 10 levels of black (16 limited / 0 full range), its stddev is <= 3.5
  and < 1 % of its pixels are 32+ levels above black. Encoded bars are flat; dark film has grain;
- an all-black sample (fade through black) carries no information and is skipped;
- symmetric crop per axis (senders centre letterboxed content; an off-centre dark area is not a bar);
- a new extent must be confirmed by 2 consecutive samples; the reported rect is the union of the last 6 samples, so
  bars shrinking (content growing) shows within ~1 s, while bars growing needs ~3 s of consistent evidence: a dark
  scene cannot crop content seen moments ago;
- changes < 1 % are ignored, bars < 1.2 % are not cropped, content < 15 % of the frame is rejected; the detected
  edge is pulled in by half a sample to hide the blended edge line.

## Audio
- Codecs: AAC-ELD (AudioSpecificConfig from `on_audio_format`; a default iOS ELD ASC is synthesised when the sender
  gives none), ALAC (magic cookie), AAC-LC, PCM (s16le, contract section 1). FFmpeg decode -> swresample to float stereo at the mastering
  voice rate (usually 48 kHz) -> one XAudio2 source voice (`AudioCategory_Media`).
- Timing (`portable/AudioScheduler`): a sample with pts P plays at P + D, where
  D = max(arrival lag + 50 ms jitter target, current video latency) + `audioOffsetMs`. Arrival lag is a slowly
  decaying maximum of (arrival - pts). The write cursor is now + (submitted - played) / rate. Each decoded chunk is
  compared with the cursor: early -> silence inserted, late -> the start skipped (or the chunk dropped), within
  +-20 ms nothing changes. Every correction and every restart after an underrun fades in over 5 ms (no clicks).
- Underrun: the voice running dry is counted (`underruns`) and the next chunk re-anchors the timeline with a fade in.
- Volume: `OnVolume(db)`: -144 = mute, -30..0 dB -> linear gain on the source voice. `SetAudioEnabled(false)` fades
  out and stops submitting (packets are still parsed). Flush / format change fade the queued tail and recreate the
  voice. Engine loss (`OnCriticalError`, e.g. HDMI/headset change) rebuilds the engine at the new device rate.

## Latency budget (iPhone -> TV, mirroring)
| Stage | Budget | How it is measured |
|---|---|---|
| iPhone capture + encode + Wi-Fi | 40-80 ms | not ours; visible in `latencyMs` minus our stages |
| castcore reassembly / decrypt | < 2 ms | castcore stats |
| decode FIFO wait | 0 ms (queue 0) | `video.decodeQueue`, `droppedRender` |
| decode (in -> out) | 2-6 ms hw | `decodeInOutMs`, `decodeMs`, `decodeHeldMax` must be 0 |
| mailbox -> draw | 0-16.7 ms | waits for a free swap chain buffer (max latency 1) |
| present -> scan-out | 1 refresh + compositor | DWM composes at vblank; not visible to the app |
| **app-visible total** | | `latencyMs` = time Present() returns - frame pts (xc_now_ns clock, castcore maps the sender clock to it) |
| audio | matches video | `audio.targetMs` follows max(jitter target, video latency); `syncErrorMs` |

Off-device numbers (Linux harness, software decode, 1 thread, 1920x886 60 fps iPhone-like stream):
decode in -> out 2.6 ms avg / 6.0 ms max, 0 packets held with the SPS rewrite; a strict decoder WITHOUT the
rewrite holds 5 packets (84 ms at 60 fps, the effect screen-mirror-xbox measured), with it 0. FFmpeg's own h264
decoder is already 1-in-1-out on this stream (it infers `has_b_frames` from the stream): the rewrite is
insurance against strict decoder paths. Audio scheduler on a simulated jittery 44.1 kHz ELD stream: target delay
84 ms (34 ms arrival lag + 50 ms), max sync error 2 ms, 0 underruns after warm-up. Compared with airplay-xbox
(200-300 ms, deliberate 200 ms buffer), the only fixed buffering here is the 50 ms audio jitter target; video has
none beyond one swap chain buffer.

## On-console verification (read `GET /status` -> `media`)
Top level (also shown by the UI stats panel): `latencyMs`, `latencyMsMax`, `decodeFps`, `renderFps`, `decodeMs`,
`decodeMsMax`, `decodeInOutMs`, `dropped`, `codec` (e.g. `"h264 d3d11va"`), `size`, `cropRect`, `mode`,
`audioBufferMs`, `underruns`. Detail: `media.video.{decoder, pixelFormat, colorMatrix, codedSize, viewSize,
inputFps, decodeHeldMax, spsRewritten, presentMsAvg, decodeQueue, renderQueue, droppedQueue, droppedRender,
decodeErrors, crop}` and `media.audio.{codec, rate, channels, outputRate, bufferMs, targetMs, arrivalLagMs,
syncErrorMs, offsetMs, volumeDb, underruns, resyncs, dropped}`.
Checks: `codec` ends in `d3d11va`; `decodeHeldMax` 0; `decodeInOutMs` < 8; `renderFps` ~= `inputFps`; `dropped`
not growing; `latencyMs` steady (expect ~20-40 ms app-visible); rotate the phone: `size` flips, no garbage frames;
play a 16:9 video in portrait: `cropRect` shrinks to the video within ~3 s and `mode` reads `Auto (Fill)`;
`underruns` stays 0; HEVC stream (if the sender picks it): `pixelFormat` p010 for 10-bit.
Logs: tags `video`, `render`, `audio` (setup, every format change, 10 s summaries, every fallback).

## Requests to platform
1. **Exclude `src\media\test\**` from the wildcards** (ClCompile / ClInclude): it holds the Linux harness
   (`*.cc`, CMake, a fallback `castcore_types_fallback.h`, its own `pch.h`). The `.cc` files are not matched by
   `*.cpp`, but the `.h` files are picked up as ClInclude and `test\pch.h` must never shadow `app\pch.h`.
2. FFmpeg's `libavutil/hwcontext_d3d11va.h` is used (the 8.1.2 NuGet package ships it); no extra libs beyond the
   documented set (`avcodec avutil swresample`, `d3d11 dxgi xaudio2`).

## Requests to ui
- `ResetPan()` exists now (additive to contract section 3); the bridge can call it instead of the two `Pan` calls.
- The `media` status section is flat at top level (fields above) with `video`/`audio` objects for detail, so the
  stats panel's one-level flattening shows the useful fields.

## Third party
Approaches and some code follow these GPL-3.0 projects (attribution in the file headers):
| Project | Used for |
|---|---|
| moonlight-xbox (github.com/TheElixZammuto/moonlight-xbox) | D3D11VA on the app's device with lock callbacks, decoder-surface SRV pool, waitable swap chain + max latency 1, inverse composition scale |
| screen-mirror-xbox (github.com/vproHacks/screen-mirror-xbox) | diagnosis of the iOS SPS/VUI reorder delay (docs/DEVLOG.md) |
| airplay-xbox (github.com/CansDE/airplay-xbox) | reference for the D3D11VA + software fallback path and XAudio2 output on Xbox |

## Known gaps
- Present(1,0) vs Present(0,0) not measured on hardware yet (`SetSyncInterval` exists for the A/B).
- HDR (BT.2020 PQ) output is not done: 10-bit HEVC is tone-mapped by nothing, just converted with the BT.2020 matrix
  into the SDR swap chain.
- AAC-ELD is verified with a conformance stream and our synthesised ASC; a real iPhone ELD stream is verified by the
  core harness / on console.
- Audio clock drift (sender vs Xbox crystal) is corrected only by the +-20 ms resync; no fine-grained resampling.
