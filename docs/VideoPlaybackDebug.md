# Debug video playback

Use a separate capture directory for each game and each run. Keep the emulator's
log filter at `*:Info` or include `Service:Info HW.GPU:Info`. Do not enable the
experimental EOS-release override for the first run.

## Capture the failing scene

In PowerShell, before launching the debug build:

```powershell
New-Item -ItemType Directory -Force C:\video-debug\decoded, C:\video-debug\presented
$env:EDEN_NX2_VDEC_TRACE = "1"
$env:EDEN_NX2_VDEC_DUMP = "C:\video-debug\decoded"
$env:EDEN_NX2_FRAME_CAPTURE = "C:\video-debug\presented"
$env:EDEN_NX2_FRAME_CAPTURE_EVERY = "30"
Remove-Item Env:EDEN_NX2_VDEC_EOS_RELEASE_NOW -ErrorAction SilentlyContinue
```

On Linux:

```sh
mkdir -p "$HOME/video-debug/decoded" "$HOME/video-debug/presented"
export EDEN_NX2_VDEC_TRACE=1
export EDEN_NX2_VDEC_DUMP="$HOME/video-debug/decoded"
export EDEN_NX2_FRAME_CAPTURE="$HOME/video-debug/presented"
export EDEN_NX2_FRAME_CAPTURE_EVERY=30
unset EDEN_NX2_VDEC_EOS_RELEASE_NOW
```

1. Launch the emulator from that shell and reproduce the movie without skipping it.
2. Save the complete emulator log and both capture directories.
3. Restart the emulator, select a new capture directory, and repeat while skipping the movie.
4. Record the game ID, build commit, GPU driver, video-decoder setting, and the point where you skipped.

For Yoshi, capture both the title movie and the story intro after New Game.
For Kirby Air Riders, capture from before entering the cutscene through the crash.

## Read the trace

Search the log for `nx2:vdec trace`.

- `submit` records packet size, timestamp, EOS, and the input queue length. Eight queued packets suppress the readiness signal but do not reject a submission, except in Ocarina of Time, whose player ran slow while its demuxer queued whole movies: there a ninth packet is refused with `0x1c266f` and logged as `input full`. `EDEN_NX2_VDEC_INPUT_LIMIT=<n>` sets that limit for any game (0: none). Older builds logged `input full` and rejected packets; Mario Tennis advances past those packets instead of retrying, corrupting VP9 reference frames.
- `decode` records codec, whether FFmpeg opened, pictures produced, the total decoded-frame count, and whether a flush discards those pictures.
- `write` records the output image, CPU address, GPU address, device address, layout, timestamp, and EOS. `gpu_known=false` means no GPU address was established, not that GPU address zero is valid.
- `present` compares the presented image and device address with the latest decoded image write. `same_image` and `same_device` are direct identity comparisons, not proof that the game sampled the video texture. A game can blit the movie into another surface, and a ring can present an older decoded image.
- `release wait`, `handout`, `output`, `output empty`, `flush`, `stop`, `complete fences`, `synthetic EOS`, and `destroy` show the skip/teardown sequence and fence progress.

Decoded PGM captures include the output image ID in their filenames so concurrent
movies do not overwrite one another's captures. They sample the first channel every 120 output frames. For NV12
that channel is luma; for RGBA/BGRA it is one color channel, not a full-color image.
GPU-read block-linear output is captured before swizzling. Presented TGA captures
show what the renderer displays. A nonblack decoded capture beside a black TGA
narrows the failure to the path after decoding, but does not identify the failing GPU pass by itself.

## Compare synchronization and decoder modes

Only after saving the baseline, repeat with `EDEN_NX2_SYNC_SUBMIT=1`.
This enables #123's bounded wait after GPU submissions. It is off by default
because the fork already snapshots command buffers. It can reduce performance
or log a two-second timeout when the GPU needs more guest work.
Unset it or set it to `0` for the normal path.

Repeat with the emulator's video-decoder setting on CPU and then GPU.
Keep the logs separate. AV1 requires an FFmpeg build with an AV1 decoder;
the existing MSVC/Linux AV1 package selection does not establish MinGW support.

Unset all trace/capture variables after testing. `EDEN_NX2_VDEC_TRACE` is enabled
by presence, so setting it to `0` does not disable it. Restart after changing
environment variables because the emulator caches them.

## Ocarina of Time: how the movie player keeps time

Read from the game's code (main, movie player around `0x3b5c000`-`0x3b62200`):

- With an audio track, the player opens an `nn::audio` output stream (sysmix, 20 ms ring, notified
  when 10 ms are free) and advances the video by `round((A - V) / frame / 2 + 1)` frames per game
  frame, where A is the timestamp of the audio frame being written and V the shown video frame.
- Without one (the intro and the title screen: no sysmix output stream is opened in the logs), it
  adds the game's fixed frame step (`60 / N`, N an integer frame rate, so 1/60 s at 60 fps; not a
  measured time) to an accumulator, advances `floor(acc / (60 / fps))` frames and subtracts them
  from the accumulator before it knows whether the frames are there. A frame missing from the
  player's queue at that moment is never made up: the video falls one frame further behind the
  music, which the game plays in real time through the audio renderer.
- The decoded frame reaches that queue through several guest threads: the player's handler releases
  the shown frame, the movie library fetches the next one (vdecsvc 213, then 314), signals its
  output event, the player's dispatcher posts a message and the handler acquires the frame.

In the intro (log of build oot-latest2, limit 64): 9297 presents, 140 game frames that released no
video frame and 62 that released two, so 78 frames lost, 1.30 s, the measured drift. The decoder
writes each frame about 1 ms after the release that frees its image, so the loss is in the guest
chain above, which only falls behind while the game reads the movie file.
`tools/nx2/patches/ipc-trace-threads.patch` adds the calling thread, its core and its priority to
`EDEN_NX2_IPC_TRACE` lines, to find which thread of that chain is late and what holds its core.

With that trace (intro, 61 frames lost): the movie library's decoder thread (core 5, priority 45)
submits packets, releases images (314) and arms the fence (syncpt 201); its event-manager thread
(any core, priority 44) fetches each frame (213) once the player has handed back the previous one.
Both calls land within 2 ms before the present, and the input limit rejected only 172 packets in the
whole run, so neither the decoder nor the limit is late. The late link is one of the player's own
steps (acquire after the output event, release after Advance), which make no service calls.
`tools/nx2/patches/sched-late-run-trace.patch` adds `EDEN_NX2_SCHED_TRACE=<ms>`: every guest thread
that waits longer than that between becoming runnable and running is logged with the thread its core
was running at that moment.
