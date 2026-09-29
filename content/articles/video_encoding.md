+++
title = "Built-in Video Encoding Using FFmpeg"
template = "article"
date = "August 31, 2026"
description = "Directly generate videos and images from the renderer."
draft = true
+++

*{{date}}*
# {{title}}

As I have begun making these posts, I have found myself with a need to take screenshots and videos of my renders. I have found the workflow of
screen recorders to be cumbersome and to produce lackluster results, so I had the idea to generate output directly from my renderer and encode the
images into a video format.

[FFmpeg](https://www.ffmpeg.org/) is a very popular, open-source software for encoding, decoding, analyzing, and manipulating audio and video formats.
It is used in many popular media programs and is quite tried and tested. It is also quite straightforward to use.

# Taking Screenshots

Before getting into video encoding, let's try tackling still image generation first. It is a simpler problem, and is a prerequisite to being able
to turn many screenshots into a video.

Currently, the renderer draws into the current frame's draw image. After all the rendering is done, the draw image is copied into a swapchain image and
the swapchain image is then submitted into the GPU's present queue for presentation to the screen. We still want this to happen, since we want to see
what we will be recording, but we also want to take that finished draw image and save it to a file.

## Output Image

My first instinct was to just read what's in the draw image, then call an [stb image](https://github.com/nothings/stb/blob/master/stb_image.h)
function on its data. However, there are a few issues with this.

1) The draw image is recreated whenever we resize the window. This means that the size of the static image will be completely dependent on the extent
of the swapchain, and when we eventually want to output a video, the frames of the video may differ in size.

2) The draw image is not currently accessible from the CPU since it is stored in GPU-only memory. We would like it to stay this way for performance
reasons.

3) The renderer waits on each frame's draw image to be done. Outputting a still from this image adds another dependency and synchronization
that is extraneous to rendering.

So instead, let's create a separate image that is accessible from the CPU and copy the draw image into it after it is released by the drawing step.
We can specify the extent of this output image separately, and once we have the image copied over, the renderer is free to reuse the draw image, and
we can do whatever we like with the copied image on the CPU.

