# Ocarina of Time: the white fog in the Deku Tree cutscene

Status of the investigation into the washed-out haze in The Legend of Zelda: Ocarina of Time
(`0400486024DEC000`), Deku Tree cutscene ("Je sens que... mon existence touche à sa fin"). The
reference picture is darker and bluer, with a light haze; ours is a white veil over everything past a
few metres, the sky hidden.

## What is established

From a sampled frame dump of Lichii nx2 + port/ducky-oot-nx2 (frame 10890), without the
`EDEN_NX2_OOT_VISUAL_WORKAROUND`:

| Step | Program (frame dump hash / native key) | Finding |
| --- | --- | --- |
| Scene before fog | passes 70-72 | Correct: dark tree, sky, rocks. |
| Fog light pass | dispatch 80, `75d4369f3b1f5868` / `d9b1657f3a0b4bc2` | Writes 64x36x96 and 64x36x48 volumes. |
| Second pass | dispatch 81, `32c1819485153ec3` / `0c546119f1c74fbd` | 64x36x48 volume. |
| Injection | dispatch 82, `6a6f4e8d501a39c7` / `e883414e8c9be33b` | 128x72x96 in-scatter (2x B10G11R11) and extinction (R16F). Extinction is 0.01-0.04 near, then a mean of 0.32 at slice 32 and 0.16 at slice 48, many voxels at exactly 1.0. |
| Integration | dispatch 83, `780c0536b89fc89e` / `ec68e40eb783ff32` | Optical depth reaches 7.1 at the far end (transmittance e^-7): the fog is opaque from mid distance. |
| Composite | pass 73, fragment `3ddfb1d7a854c849` / `4b2adcc4d5577ed1` | The one draw where the picture turns white. |
| Sky ambient | `b76d7b3e9e93093d` / `799e4ced3a1e4961` | Float atomic adds (RED.ADD.F32) of 12 sky SH floats read by the fog and materials. |

Ruled out, with the evidence:

- **Ping-pong feedback.** Dispatch 82 reads the history volumes at the addresses dispatch 83 writes,
  but their content before dispatch 82 is an injected volume (same profile as dispatch 82's output),
  not an integrated one: the sets alternate per frame.
- **Shadow maps.** The D16 1600x1600 (near plants) and D32 4096x4096 (canopy from the sun) maps have
  plausible content.
- **Composite depth.** The composite samples a D32 2560x1440 target that holds only the water
  surfaces. `EDEN_NX2_TRACE_DST` shows the game clears it and draws only the water into it; that is by
  design. The fog depth comes from a linear depth (R32 2560x1440): view depth = tex.x * c3[0x9e8] +
  c3[0x9d0], clamped to c4[0x5f0]..c4[0x5f4], slice = (ln(d/near) / ln(far/near))^(1/c4[0x5f8]) *
  c4[0x608]. Its values (tree 0.0009, near ground 0.0001, sky 1.0) are consistent.
- **F2I constant-buffer abs/neg** (fix in Lichii `1ad14bebb8`): no captured program uses the form.
- **The lowering of the newly accepted particle programs** (`0051f3226c2ac35e`, `e311d97dbe057ad3`):
  rejecting them changes nothing in the fog.

## The strongest lead

The OoT PC static recompilation (philneitz/oot-pc-recomp, branch `oot/merged`) has the same symptom
with an entirely different renderer (a host NVN 2 implementation and a Ryujinx-based translator):
"Dropping the fog composite removes the veil, so it comes from there; why the fog's even part is so
strong is not known." The only component the two projects share is Eglantine's Ampere to Maxwell
lowering. So a silently miscomputed instruction in one of the five programs above is the most likely
cause, ahead of anything in Lichii's GPU emulation.

Checked so far in those programs: F2IP, UIADD3 and ULEA carries, FFMA |a| (bit 73), RED.ADD.F32
(type 3). All lower as the encoding describes.

## Next steps

1. A sampled frame dump with the constant and storage buffers
   (`tools/nx2/patches/frame-dump-buffers-on-lichii-nx2.patch`, `EDEN_NX2_FRAME_DUMP_SAMPLED=1`).
   With c3/c4 the slice of the tree pixels and the optical depth there can be computed, and the
   sky SH buffer checked for accumulation across frames (it must be cleared each frame).
2. Audit the five programs instruction by instruction against nvdisasm semantics, starting with the
   injection program's density math (extinction pinned at 1.0 suggests a saturate or clamp on a value
   that should stay small).
3. Report the finding to the recomp project as well; the fix applies to both.
