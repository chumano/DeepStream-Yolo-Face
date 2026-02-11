# FFMpeg

```bash
# Extract frames from video at 3 FPS
ffmpeg -i ./videos/faces_tracking.mp4 -vf fps=3 ./videos/frames/faces_tracking_%04d.jpg
```