# Identity GBR FFmpeg Requirement

Linux builds that advertise PLANK identity GBR must link against an
FFmpeg build containing `0001-hevc-enable-hwaccel-for-identity-gbr.patch`.
Without it, FFmpeg exposes only the `gbrp10le` software format for HEVC streams
whose VUI uses matrix coefficients 0, even when the VA-API driver supports the
Main 4:4:4 10 profile.

Apply the patch to the FFmpeg source used by the Moonlight package. Do not
replace distribution FFmpeg libraries globally; package the patched libraries
with Moonlight or build them in an isolated prefix.
