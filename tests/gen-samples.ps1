# Generates the PLAN.md test-matrix files into tests/samples/.
# Requires an ffmpeg CLI on PATH (only for generating test inputs;
# it is not part of the shipped player). e.g. winget install Gyan.FFmpeg
$ErrorActionPreference = "Stop"
$out = Join-Path $PSScriptRoot "samples"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$video = "testsrc2=duration=30:size=1280x720:rate=30"
$audio = "sine=frequency=440:duration=30"

Write-Host "h264 + aac in mp4"
ffmpeg -y -f lavfi -i $video -f lavfi -i $audio -c:v libx264 -preset veryfast `
    -c:a aac -shortest "$out\h264_aac.mp4"

Write-Host "hevc + ac3 in mkv"
ffmpeg -y -f lavfi -i $video -f lavfi -i $audio -c:v libx265 -preset veryfast `
    -c:a ac3 -shortest "$out\hevc_ac3.mkv"

Write-Host "vp9 + opus in webm"
ffmpeg -y -f lavfi -i $video -f lavfi -i $audio -c:v libvpx-vp9 -deadline realtime `
    -c:a libopus -shortest "$out\vp9_opus.webm"

Write-Host "mpeg4 + mp3 in avi"
ffmpeg -y -f lavfi -i $video -f lavfi -i $audio -c:v mpeg4 -c:a libmp3lame `
    -shortest "$out\mpeg4_mp3.avi"

Write-Host "srt sidecar + embedded"
@"
1
00:00:01,000 --> 00:00:05,000
Hello subtitle world

2
00:00:06,000 --> 00:00:10,000
Second cue, line one
and line two
"@ | Set-Content -Encoding UTF8 "$out\h264_aac.srt"

ffmpeg -y -i "$out\h264_aac.mp4" -i "$out\h264_aac.srt" -c copy -c:s srt `
    "$out\embedded_srt.mkv"

Write-Host "done -> $out"
