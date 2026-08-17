+++
title = "My Little Renderer"
template = "article"
date = "April 13, 2026"
description = "A home-brew Vulkan rendering API for personal use."
draft = true
+++

# {{title}}

I am haunted by a curse. One that plagues every young, aspirational programmer with a thirst for knowledge and a hunger to
build. Reaching for existing code never satisfies; it's a solved problem, and solved problems are boring.
I want&mdash;no, I **need**&mdash;the satisfaction, the prestige, the bragging rights that come of writing it
all **from scratch**.

Thus, as I found needing to render images, naturally, I wrote my own Vulkan renderer.^[Yes, the irony of using an existing library here is not lost on me, but the buck has to stop somewhere.
I am not enough of a masochist to write my own GPU drivers.]

I wanted to be able to render to the screen and develop my own windowed apps without having to write all the rendering API boilerplate every time. So I designed the renderer to be a
light abstraction over Vulkan to make my life easier while still leveraging Vulkan's flexibility and control.

There are a plethora of great resources out there to learn how to use Vulkan effectively. You will see a lot of similarities between my code and the incredible [VulkanGuide](https://vkguide.dev/).
If you are reading this, you are also probably aware of Sascha Willems' [Vulkan examples and demos](https://github.com/SaschaWillems/Vulkan), which are incredibly invaluable as learning resources.

## Overview
